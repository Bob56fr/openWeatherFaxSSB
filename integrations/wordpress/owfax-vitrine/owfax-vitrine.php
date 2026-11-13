<?php
/**
 * Plugin Name:       openWeatherFaxSSB — Vitrine
 * Plugin URI:        https://github.com/Bob56fr/openWeatherFaxSSB
 * Description:       Replays the weather fax charts received by an openWeatherFaxSSB station, slowly, with their real sound, S-meter and signal quality.
 * Version:           0.1.0
 * Requires at least: 6.0
 * Requires PHP:      7.4
 * Author:            openWeatherFaxSSB contributors
 * License:           AGPL-3.0-or-later
 * License URI:       https://www.gnu.org/licenses/agpl-3.0.html
 * Text Domain:       owfax-vitrine
 * Domain Path:       /languages
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 openWeatherFaxSSB contributors
 *
 * Web showcase of the station (docs/VITRINE-WEB.md § 6): the station pushes complete
 * charts, the site replays them. No live stream, no connection to the station.
 */

defined( 'ABSPATH' ) || exit;

define( 'OWFAX_VITRINE_VERSION', '0.1.0' );
define( 'OWFAX_VITRINE_FILE', __FILE__ );
define( 'OWFAX_VITRINE_DIR', plugin_dir_path( __FILE__ ) );

require_once OWFAX_VITRINE_DIR . 'includes/class-schema.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-rules.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-store.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-charts.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-purge.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-ingest.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-front.php';
require_once OWFAX_VITRINE_DIR . 'includes/class-admin.php';

final class Owfax_Vitrine_Plugin {
	const CAP      = 'owfax_publish';
	const ROLE     = 'owfax_publisher';
	const SETTINGS = 'owfax_vitrine_settings';
	const REFUSALS = 'owfax_vitrine_refusals';

	public static function init(): void {
		add_action( 'init', array( __CLASS__, 'load_textdomain' ), 1 );
		Owfax_Vitrine_Charts::init();
		Owfax_Vitrine_Purge::init();
		Owfax_Vitrine_Ingest::init();
		Owfax_Vitrine_Front::init();
		Owfax_Vitrine_Admin::init();
		register_activation_hook( OWFAX_VITRINE_FILE, array( __CLASS__, 'activate' ) );
		register_deactivation_hook( OWFAX_VITRINE_FILE, array( __CLASS__, 'deactivate' ) );
	}

	public static function load_textdomain(): void {
		load_plugin_textdomain( 'owfax-vitrine', false, dirname( plugin_basename( OWFAX_VITRINE_FILE ) ) . '/languages' );
	}

	/** "20261110T1007Z_DDK6_13882.5kHz": also safe as a directory name. */
	public static function is_chart_id( string $s ): bool {
		return 1 === preg_match( '/^[0-9]{8}T[0-9]{4}Z_[A-Za-z0-9-]+_[0-9.]+kHz$/', $s ) && false === strpos( $s, '..' );
	}

	public static function defaults(): array {
		return array(
			'quota_mb'            => 2048,   // WPP-010
			'max_age_days'        => 60,
			'max_count'           => 300,
			'moderation'          => 'auto', // PUB-025: auto or draft
			'rate_per_hour'       => 30,     // SEC-003
			'delete_on_uninstall' => false,  // WPP-044
		);
	}

	public static function settings(): array {
		return array_merge( self::defaults(), (array) get_option( self::SETTINGS, array() ) );
	}

	/** @return array{quota_bytes: int, max_age_days: int, max_count: int} */
	public static function limits(): array {
		$s = self::settings();
		return array(
			'quota_bytes'  => (int) $s['quota_mb'] * 1024 * 1024,
			'max_age_days' => (int) $s['max_age_days'],
			'max_count'    => (int) $s['max_count'],
		);
	}

	private static $schema = null;

	public static function schema(): Owfax_Vitrine_Schema {
		if ( null === self::$schema ) {
			self::$schema = Owfax_Vitrine_Schema::from_file( OWFAX_VITRINE_DIR . 'schema/owfax-vitrine-0.1.schema.json' );
		}
		return self::$schema;
	}

	/** Refused sends, the 50 latest, shown in the settings page (SEC-003). */
	public static function log_refusal( string $code, string $message, string $chart_id ): void {
		$log   = (array) get_option( self::REFUSALS, array() );
		$log[] = array( time(), $code, $message, $chart_id );
		update_option( self::REFUSALS, array_slice( $log, -50 ), false );
		if ( defined( 'WP_DEBUG_LOG' ) && WP_DEBUG_LOG ) {
			error_log( 'owfax-vitrine: refused ' . $chart_id . ': ' . $code . ': ' . $message );
		}
	}

	public static function activate(): void {
		// A dedicated account for the station: it can send charts, nothing else (SEC-002).
		add_role(
			self::ROLE,
			__( 'owfax publisher', 'owfax-vitrine' ),
			array(
				'read'    => true,
				self::CAP => true,
			)
		);
		$admin = get_role( 'administrator' );
		if ( $admin ) {
			$admin->add_cap( self::CAP );
		}
		Owfax_Vitrine_Store::ensure();
		Owfax_Vitrine_Charts::register();
		flush_rewrite_rules();
		if ( ! wp_next_scheduled( Owfax_Vitrine_Purge::CRON ) ) {
			wp_schedule_event( time() + HOUR_IN_SECONDS, 'daily', Owfax_Vitrine_Purge::CRON );
		}
	}

	public static function deactivate(): void {
		wp_clear_scheduled_hook( Owfax_Vitrine_Purge::CRON );
		flush_rewrite_rules();
	}
}

Owfax_Vitrine_Plugin::init();
