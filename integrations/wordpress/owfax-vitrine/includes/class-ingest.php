<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Ingestion of chart packages sent by the station (WPP-001, PUB-060, PUB-061,
// PUB-064, SEC-002, SEC-003):
//
//   POST /wp-json/owfax/v1/charts   multipart: meta, telemetry, raw, enhanced, thumb, audio
//   GET  /wp-json/owfax/v1/limits   upload limits, quota and space used
//
// Authentication: a WordPress application password of an account that holds the
// owfax_publish capability (the "owfax publisher" role has nothing else).

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Ingest {
	const NS = 'owfax/v1';

	public static function init(): void {
		add_action( 'rest_api_init', array( __CLASS__, 'routes' ) );
	}

	public static function routes(): void {
		register_rest_route(
			self::NS,
			'/charts',
			array(
				'methods'             => 'POST',
				'callback'            => array( __CLASS__, 'post_chart' ),
				'permission_callback' => array( __CLASS__, 'allowed' ),
			)
		);
		register_rest_route(
			self::NS,
			'/limits',
			array(
				'methods'             => 'GET',
				'callback'            => array( __CLASS__, 'get_limits' ),
				'permission_callback' => array( __CLASS__, 'allowed' ),
			)
		);
	}

	public static function allowed(): bool {
		return current_user_can( Owfax_Vitrine_Plugin::CAP );
	}

	private static function refuse( int $status, string $code, string $message, string $chart_id = '' ): WP_Error {
		Owfax_Vitrine_Plugin::log_refusal( $code, $message, $chart_id );
		return new WP_Error( $code, $message, array( 'status' => $status ) );
	}

	/** At most N sends per hour (SEC-003); counts every attempt, refused or not. */
	private static function rate_ok(): bool {
		$limit = Owfax_Vitrine_Plugin::settings()['rate_per_hour'];
		$now   = time();
		$times = array_filter(
			(array) get_option( 'owfax_vitrine_sends', array() ),
			function ( $t ) use ( $now ) {
				return $t > $now - 3600;
			}
		);
		if ( count( $times ) >= $limit ) {
			return false;
		}
		$times[] = $now;
		update_option( 'owfax_vitrine_sends', array_values( $times ), false );
		return true;
	}

	public static function get_limits(): WP_REST_Response {
		$files = array();
		foreach ( Owfax_Vitrine_Rules::files() as $field => $f ) {
			$files[ $field ] = array(
				'name'      => $f[0],
				'max_bytes' => $f[1],
				'required'  => $f[3],
			);
		}
		$limits = Owfax_Vitrine_Plugin::limits();
		$items  = Owfax_Vitrine_Charts::items();
		return new WP_REST_Response(
			array(
				'max_upload_bytes' => (int) wp_max_upload_size(),
				'max_post_bytes'   => (int) wp_convert_hr_to_bytes( (string) ini_get( 'post_max_size' ) ),
				'files'            => $files,
				'quota_bytes'      => $limits['quota_bytes'],
				'used_bytes'       => array_sum( array_column( $items, 'bytes' ) ),
				'charts'           => count( $items ),
				'plugin_version'   => OWFAX_VITRINE_VERSION,
			)
		);
	}

	/** @return WP_REST_Response|WP_Error */
	public static function post_chart( WP_REST_Request $request ) {
		if ( ! self::rate_ok() ) {
			return self::refuse( 429, 'owfax_rate', 'too many sends in the last hour' );
		}
		$specs = Owfax_Vitrine_Rules::files();
		$files = $request->get_file_params();
		foreach ( array_keys( $files ) as $field ) {
			if ( ! isset( $specs[ $field ] ) ) {
				return self::refuse( 400, 'owfax_unexpected_file', 'unexpected file: ' . $field );
			}
		}
		if ( $request->get_body_params() ) {
			return self::refuse( 400, 'owfax_unexpected_field', 'only files are expected' );
		}
		$bytes = 0;
		foreach ( $specs as $field => $spec ) {
			if ( ! isset( $files[ $field ] ) ) {
				if ( $spec[3] ) {
					return self::refuse( 400, 'owfax_missing_file', 'missing file: ' . $field );
				}
				continue;
			}
			$f = $files[ $field ];
			if ( is_array( $f['error'] ) || UPLOAD_ERR_OK !== (int) $f['error'] ) {
				return self::refuse( 400, 'owfax_upload_error', 'upload failed: ' . $field );
			}
			if ( (int) $f['size'] <= 0 || (int) $f['size'] > $spec[1] ) {
				return self::refuse( 413, 'owfax_too_big', $field . ': ' . (int) $f['size'] . ' bytes, at most ' . $spec[1] );
			}
			if ( ! is_uploaded_file( $f['tmp_name'] ) || ! Owfax_Vitrine_Rules::sniff( $f['tmp_name'], $spec[2] ) ) {
				return self::refuse( 415, 'owfax_bad_type', $field . ': not a ' . $spec[2] . ' file' );
			}
			$bytes += (int) $f['size'];
		}

		$meta_text = (string) file_get_contents( $files['meta']['tmp_name'] );
		$meta      = json_decode( $meta_text );
		$errors    = Owfax_Vitrine_Rules::check_meta( $meta, Owfax_Vitrine_Plugin::schema() );
		if ( $errors ) {
			$id = is_object( $meta ) && isset( $meta->chart_id ) && is_string( $meta->chart_id ) ? $meta->chart_id : '';
			return self::refuse( 422, 'owfax_bad_meta', implode( '; ', array_slice( $errors, 0, 5 ) ), $id );
		}
		$chart_id = $meta->chart_id;
		$errors   = Owfax_Vitrine_Rules::check_telemetry( json_decode( (string) file_get_contents( $files['telemetry']['tmp_name'] ) ) );
		if ( $errors ) {
			return self::refuse( 422, 'owfax_bad_telemetry', implode( '; ', $errors ), $chart_id );
		}

		// Idempotent on chart_id (PUB-061): a chart sent again is not stored twice.
		$existing = Owfax_Vitrine_Charts::find( $chart_id );
		if ( $existing ) {
			return new WP_REST_Response( self::answer( $existing, 'exists' ), 200 );
		}

		// Room first, then the files (WPP-012); 507 when the charts to keep fill it (WPP-014).
		if ( ! Owfax_Vitrine_Purge::make_room( $bytes ) ) {
			return self::refuse( 507, 'owfax_no_room', 'no room left: the charts to keep fill the quota', $chart_id );
		}

		// Written next to the final place, then renamed: never half a package online.
		Owfax_Vitrine_Store::ensure();
		$final = Owfax_Vitrine_Store::chart_dir( $chart_id );
		$tmp   = Owfax_Vitrine_Store::base_dir() . '/.incoming-' . wp_generate_password( 12, false );
		if ( ! wp_mkdir_p( $tmp ) ) {
			return self::refuse( 500, 'owfax_storage', 'cannot write in uploads/owfax', $chart_id );
		}
		foreach ( $specs as $field => $spec ) {
			if ( isset( $files[ $field ] ) && ! move_uploaded_file( $files[ $field ]['tmp_name'], $tmp . '/' . $spec[0] ) ) {
				Owfax_Vitrine_Store::remove_tree( $tmp );
				return self::refuse( 500, 'owfax_storage', 'cannot store ' . $spec[0], $chart_id );
			}
		}
		if ( is_dir( $final ) ) {
			Owfax_Vitrine_Store::remove_tree( $final );  // left by an interrupted ingestion
		}
		if ( ! @rename( $tmp, $final ) ) {
			Owfax_Vitrine_Store::remove_tree( $tmp );
			return self::refuse( 500, 'owfax_storage', 'cannot move the package in place', $chart_id );
		}

		$status = 'draft' === Owfax_Vitrine_Plugin::settings()['moderation'] ? 'draft' : 'publish';
		$post   = Owfax_Vitrine_Charts::create( $meta, $meta_text, $bytes, $status );
		if ( is_wp_error( $post ) ) {
			Owfax_Vitrine_Store::remove_tree( $final );
			return self::refuse( 500, 'owfax_post', $post->get_error_message(), $chart_id );
		}
		Owfax_Vitrine_Purge::log_ingestion( $bytes );
		return new WP_REST_Response( self::answer( get_post( $post ), 'created' ), 201 );
	}

	private static function answer( WP_Post $post, string $result ): array {
		return array(
			'chart_id'    => (string) get_post_meta( $post->ID, Owfax_Vitrine_Charts::META_ID, true ),
			'result'      => $result,
			'post_id'     => $post->ID,
			'post_status' => $post->post_status,
			'url'         => get_permalink( $post ),
			'bytes'       => (int) get_post_meta( $post->ID, Owfax_Vitrine_Charts::META_BYTES, true ),
		);
	}
}
