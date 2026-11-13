<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// The owfax_chart content type: one entry per published chart, page at
// /fax/<chart id>/ (WPP-004, WPP-005, WPP-021). Entries are only created by the
// ingestion endpoint, never by hand.

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Charts {
	const TYPE = 'owfax_chart';

	/** Post meta, indexed for sorting and filters (WPP-004). */
	const META_ID       = '_owfax_chart_id';
	const META_STATION  = '_owfax_station_id';
	const META_START    = '_owfax_start';      // Unix time of the chart start
	const META_KHZ      = '_owfax_khz';
	const META_DISTANCE = '_owfax_distance_km';
	const META_QUALITY  = '_owfax_quality';
	const META_BYTES    = '_owfax_bytes';
	const META_KEEP     = '_owfax_keep';
	const META_JSON     = '_owfax_meta';       // the whole meta.json, for the page

	public static function init(): void {
		add_action( 'init', array( __CLASS__, 'register' ) );
		add_action( 'before_delete_post', array( __CLASS__, 'on_delete' ), 10, 1 );
		add_action( 'add_meta_boxes_' . self::TYPE, array( __CLASS__, 'meta_box' ) );
		add_action( 'save_post_' . self::TYPE, array( __CLASS__, 'save_keep' ), 10, 2 );
		add_filter( 'manage_' . self::TYPE . '_posts_columns', array( __CLASS__, 'columns' ) );
		add_action( 'manage_' . self::TYPE . '_posts_custom_column', array( __CLASS__, 'column' ), 10, 2 );
		add_filter( 'manage_edit-' . self::TYPE . '_sortable_columns', array( __CLASS__, 'sortable' ) );
		add_action( 'pre_get_posts', array( __CLASS__, 'admin_sort' ) );
	}

	public static function register(): void {
		register_post_type(
			self::TYPE,
			array(
				'labels'       => array(
					'name'          => __( 'Weather fax charts', 'owfax-vitrine' ),
					'singular_name' => __( 'Weather fax chart', 'owfax-vitrine' ),
					'menu_name'     => __( 'Weather fax', 'owfax-vitrine' ),
					'all_items'     => __( 'Charts', 'owfax-vitrine' ),
					'edit_item'     => __( 'Chart', 'owfax-vitrine' ),
					'search_items'  => __( 'Search charts', 'owfax-vitrine' ),
					'not_found'     => __( 'No chart received yet.', 'owfax-vitrine' ),
				),
				'public'       => true,
				'has_archive'  => false,
				'show_in_rest' => false,
				'menu_icon'    => 'dashicons-format-image',
				'supports'     => array( 'title' ),
				'rewrite'      => array(
					'slug'       => 'fax',
					'with_front' => false,
				),
				'map_meta_cap' => true,
				'capabilities' => array( 'create_posts' => 'do_not_allow' ),
			)
		);
		foreach ( array( self::META_START, self::META_BYTES ) as $key ) {
			register_post_meta( self::TYPE, $key, array( 'type' => 'integer', 'single' => true ) );
		}
		foreach ( array( self::META_KHZ, self::META_DISTANCE, self::META_QUALITY ) as $key ) {
			register_post_meta( self::TYPE, $key, array( 'type' => 'number', 'single' => true ) );
		}
		foreach ( array( self::META_ID, self::META_STATION, self::META_JSON ) as $key ) {
			register_post_meta( self::TYPE, $key, array( 'type' => 'string', 'single' => true ) );
		}
		register_post_meta( self::TYPE, self::META_KEEP, array( 'type' => 'boolean', 'single' => true ) );
	}

	/** Post of a chart id, in any status (trash included: it still holds its files). */
	public static function find( string $chart_id ): ?WP_Post {
		$q = new WP_Query(
			array(
				'post_type'      => self::TYPE,
				'post_status'    => 'any',
				'meta_key'       => self::META_ID,
				'meta_value'     => $chart_id,
				'posts_per_page' => 1,
				'no_found_rows'  => true,
			)
		);
		if ( ! $q->posts ) {
			$q = new WP_Query(
				array(
					'post_type'      => self::TYPE,
					'post_status'    => 'trash',
					'meta_key'       => self::META_ID,
					'meta_value'     => $chart_id,
					'posts_per_page' => 1,
					'no_found_rows'  => true,
				)
			);
		}
		return $q->posts ? $q->posts[0] : null;
	}

	/** Title "DWD Pinneberg — 13 882,5 kHz — 10/11/2026 14:24 UTC" (WPP-021). */
	public static function title( $meta ): string {
		$khz = (float) $meta->station->assigned_khz;
		return sprintf(
			'%s — %s kHz — %s',
			$meta->station->name,
			number_format_i18n( $khz, fmod( $khz, 1.0 ) > 0 ? 1 : 0 ),
			gmdate( 'd/m/Y H:i', (int) Owfax_Vitrine_Rules::start_unix( $meta ) ) . ' UTC'
		);
	}

	/**
	 * Creates the entry of a package whose files are in place.
	 *
	 * @param object $meta meta.json, decoded as objects.
	 * @return int|WP_Error
	 */
	public static function create( $meta, string $meta_text, int $bytes, string $status ) {
		$start = (int) Owfax_Vitrine_Rules::start_unix( $meta );
		$id    = wp_insert_post(
			array(
				'post_type'     => self::TYPE,
				'post_status'   => $status,
				'post_title'    => self::title( $meta ),
				'post_name'     => sanitize_title( $meta->chart_id ),
				'post_date_gmt' => gmdate( 'Y-m-d H:i:s', $start ),
				'post_date'     => get_date_from_gmt( gmdate( 'Y-m-d H:i:s', $start ) ),
				'post_author'   => get_current_user_id(),
				'meta_input'    => array(
					self::META_ID       => $meta->chart_id,
					self::META_STATION  => $meta->station->id,
					self::META_START    => $start,
					self::META_KHZ      => (float) $meta->station->assigned_khz,
					self::META_DISTANCE => (float) $meta->path->distance_km,
					self::META_QUALITY  => (float) $meta->quality->mean,
					self::META_BYTES    => $bytes,
					self::META_KEEP     => false,
					// wp_slash: post meta is unslashed on the way in.
					self::META_JSON     => wp_slash( $meta_text ),
				),
			),
			true
		);
		return $id;
	}

	/** meta.json of an entry, decoded as objects (null if broken). */
	public static function meta( int $post_id ) {
		$m = json_decode( (string) get_post_meta( $post_id, self::META_JSON, true ) );
		return is_object( $m ) ? $m : null;
	}

	/** A deleted entry takes its files with it (WPP-005). */
	public static function on_delete( int $post_id ): void {
		if ( get_post_type( $post_id ) !== self::TYPE ) {
			return;
		}
		$chart_id = (string) get_post_meta( $post_id, self::META_ID, true );
		if ( '' !== $chart_id ) {
			Owfax_Vitrine_Store::delete( $chart_id );
		}
	}

	/** Every entry, for the purge: id, start, bytes, keep. */
	public static function items(): array {
		global $wpdb;
		$rows  = $wpdb->get_results(
			$wpdb->prepare(
				"SELECT p.ID AS id,
					MAX(CASE WHEN m.meta_key = %s THEN m.meta_value END) AS start,
					MAX(CASE WHEN m.meta_key = %s THEN m.meta_value END) AS bytes,
					MAX(CASE WHEN m.meta_key = %s THEN m.meta_value END) AS keep
				FROM {$wpdb->posts} p LEFT JOIN {$wpdb->postmeta} m ON m.post_id = p.ID
				WHERE p.post_type = %s AND p.post_status <> 'auto-draft'
				GROUP BY p.ID",
				self::META_START,
				self::META_BYTES,
				self::META_KEEP,
				self::TYPE
			),
			ARRAY_A
		);
		$items = array();
		foreach ( (array) $rows as $r ) {
			$items[] = array(
				'id'    => (int) $r['id'],
				'start' => (int) $r['start'],
				'bytes' => (int) $r['bytes'],
				'keep'  => ! empty( $r['keep'] ),
			);
		}
		return $items;
	}

	// ------------------------------------------------------------------ admin

	public static function meta_box(): void {
		add_meta_box(
			'owfax-chart',
			__( 'Chart', 'owfax-vitrine' ),
			array( __CLASS__, 'render_box' ),
			self::TYPE,
			'normal',
			'high'
		);
	}

	public static function render_box( WP_Post $post ): void {
		$chart_id = (string) get_post_meta( $post->ID, self::META_ID, true );
		$keep     = (bool) get_post_meta( $post->ID, self::META_KEEP, true );
		wp_nonce_field( 'owfax_keep', 'owfax_keep_nonce' );
		echo '<p><img src="' . esc_url( Owfax_Vitrine_Store::chart_url( $chart_id ) . 'thumb.webp' ) .
			'" alt="" style="max-width:400px;width:100%;height:auto"></p>';
		echo '<p><code>' . esc_html( $chart_id ) . '</code> — ' .
			esc_html( size_format( (int) get_post_meta( $post->ID, self::META_BYTES, true ) ) ) . '</p>';
		echo '<p><label><input type="checkbox" name="owfax_keep" value="1"' . checked( $keep, true, false ) . '> ' .
			esc_html__( 'To keep: never purged (at most 20 % of the quota).', 'owfax-vitrine' ) . '</label></p>';
		$err = get_transient( 'owfax_keep_refused_' . $post->ID );
		if ( $err ) {
			delete_transient( 'owfax_keep_refused_' . $post->ID );
			echo '<p class="notice notice-error" style="padding:8px">' .
				esc_html__( 'Not kept: the charts to keep would exceed 20 % of the quota.', 'owfax-vitrine' ) . '</p>';
		}
	}

	public static function save_keep( int $post_id, WP_Post $post ): void {
		if ( ! isset( $_POST['owfax_keep_nonce'] ) ||
			! wp_verify_nonce( sanitize_key( wp_unslash( $_POST['owfax_keep_nonce'] ) ), 'owfax_keep' ) ||
			! current_user_can( 'edit_post', $post_id ) || wp_is_post_revision( $post_id ) ) {
			return;
		}
		$want = ! empty( $_POST['owfax_keep'] );
		$was  = (bool) get_post_meta( $post_id, self::META_KEEP, true );
		if ( $want && ! $was ) {
			$others = array_filter(
				self::items(),
				function ( $it ) use ( $post_id ) {
					return $it['id'] !== $post_id;
				}
			);
			$bytes  = (int) get_post_meta( $post_id, self::META_BYTES, true );
			if ( ! Owfax_Vitrine_Rules::keep_allowed( $others, $bytes, Owfax_Vitrine_Plugin::limits()['quota_bytes'] ) ) {
				set_transient( 'owfax_keep_refused_' . $post_id, 1, 300 );
				return;
			}
		}
		update_post_meta( $post_id, self::META_KEEP, $want );
	}

	public static function columns( array $cols ): array {
		$out = array();
		foreach ( $cols as $k => $v ) {
			if ( 'date' === $k ) {
				$out['owfax_quality']  = __( 'Quality', 'owfax-vitrine' );
				$out['owfax_distance'] = __( 'Distance', 'owfax-vitrine' );
				$out['owfax_size']     = __( 'Size', 'owfax-vitrine' );
				$out['owfax_keep']     = __( 'To keep', 'owfax-vitrine' );
			}
			$out[ $k ] = $v;
		}
		return $out;
	}

	public static function column( string $col, int $post_id ): void {
		switch ( $col ) {
			case 'owfax_quality':
				echo esc_html( round( 100 * (float) get_post_meta( $post_id, self::META_QUALITY, true ) ) . ' %' );
				break;
			case 'owfax_distance':
				echo esc_html( number_format_i18n( (float) get_post_meta( $post_id, self::META_DISTANCE, true ) ) . ' km' );
				break;
			case 'owfax_size':
				echo esc_html( size_format( (int) get_post_meta( $post_id, self::META_BYTES, true ) ) );
				break;
			case 'owfax_keep':
				echo get_post_meta( $post_id, self::META_KEEP, true ) ? '✔' : '';
				break;
		}
	}

	public static function sortable( array $cols ): array {
		$cols['owfax_quality']  = 'owfax_quality';
		$cols['owfax_distance'] = 'owfax_distance';
		$cols['owfax_size']     = 'owfax_size';
		return $cols;
	}

	public static function admin_sort( WP_Query $q ): void {
		if ( ! is_admin() || ! $q->is_main_query() || $q->get( 'post_type' ) !== self::TYPE ) {
			return;
		}
		$keys = array(
			'owfax_quality'  => self::META_QUALITY,
			'owfax_distance' => self::META_DISTANCE,
			'owfax_size'     => self::META_BYTES,
		);
		$by   = $q->get( 'orderby' );
		if ( is_string( $by ) && isset( $keys[ $by ] ) ) {
			$q->set( 'meta_key', $keys[ $by ] );
			$q->set( 'orderby', 'meta_value_num' );
		}
	}
}
