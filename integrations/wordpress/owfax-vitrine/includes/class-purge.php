<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Retention (WPP-010 … WPP-014): a chart goes with its sound, its page and its
// files. The purge runs at every ingestion, before the new package is written, and
// once a day from WP-Cron for the maximum age (WP-Cron needs visits: it only
// completes the ingestion-time purge).

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Purge {
	const CRON = 'owfax_vitrine_daily';
	const LOG  = 'owfax_vitrine_ingest_log';

	public static function init(): void {
		add_action( self::CRON, array( __CLASS__, 'daily' ) );
	}

	/** Deletes what must go so that `incoming` bytes fit; false if they cannot. */
	public static function make_room( int $incoming ): bool {
		$plan = Owfax_Vitrine_Rules::plan_purge(
			Owfax_Vitrine_Charts::items(),
			$incoming,
			Owfax_Vitrine_Plugin::limits(),
			time()
		);
		foreach ( $plan['delete'] as $post_id ) {
			wp_delete_post( $post_id, true );  // files go with it (before_delete_post)
		}
		return $plan['fits'];
	}

	public static function daily(): void {
		self::make_room( 0 );
		// Leftovers of interrupted ingestions.
		foreach ( (array) glob( Owfax_Vitrine_Store::base_dir() . '/.incoming-*', GLOB_ONLYDIR ) as $dir ) {
			if ( filemtime( $dir ) < time() - DAY_IN_SECONDS ) {
				Owfax_Vitrine_Store::remove_tree( $dir );
			}
		}
	}

	public static function log_ingestion( int $bytes ): void {
		$now = time();
		$log = array_filter(
			(array) get_option( self::LOG, array() ),
			function ( $e ) use ( $now ) {
				return is_array( $e ) && $e[0] > $now - 8 * DAY_IN_SECONDS;
			}
		);
		$log[] = array( $now, $bytes );
		update_option( self::LOG, array_values( $log ), false );
	}

	/** Space used, number of charts, oldest, projection (WPP-013). */
	public static function stats(): array {
		$items  = Owfax_Vitrine_Charts::items();
		$limits = Owfax_Vitrine_Plugin::limits();
		$used   = array_sum( array_column( $items, 'bytes' ) );
		$kept   = 0;
		$oldest = null;
		foreach ( $items as $it ) {
			$kept  += $it['keep'] ? $it['bytes'] : 0;
			$oldest = null === $oldest ? $it['start'] : min( $oldest, $it['start'] );
		}
		return array(
			'used'          => $used,
			'kept'          => $kept,
			'count'         => count( $items ),
			'oldest'        => $oldest,
			'quota'         => $limits['quota_bytes'],
			'days_to_quota' => Owfax_Vitrine_Rules::days_to_quota(
				(array) get_option( self::LOG, array() ),
				$used,
				$limits['quota_bytes'],
				time()
			),
		);
	}
}
