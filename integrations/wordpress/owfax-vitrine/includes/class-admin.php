<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Settings page: limits of the purge, moderation, send rate, uninstall choice;
// space used and projection (WPP-013), refused sends (SEC-003), and how to give the
// station its account.

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Admin {
	const PAGE = 'owfax-vitrine';

	public static function init(): void {
		add_action( 'admin_menu', array( __CLASS__, 'menu' ) );
		add_action( 'admin_init', array( __CLASS__, 'register' ) );
	}

	public static function menu(): void {
		add_submenu_page(
			'edit.php?post_type=' . Owfax_Vitrine_Charts::TYPE,
			__( 'Showcase settings', 'owfax-vitrine' ),
			__( 'Settings', 'owfax-vitrine' ),
			'manage_options',
			self::PAGE,
			array( __CLASS__, 'page' )
		);
	}

	public static function register(): void {
		register_setting(
			self::PAGE,
			Owfax_Vitrine_Plugin::SETTINGS,
			array(
				'type'              => 'array',
				'sanitize_callback' => array( __CLASS__, 'sanitize' ),
				'default'           => Owfax_Vitrine_Plugin::defaults(),
			)
		);
	}

	public static function sanitize( $in ): array {
		$in  = (array) $in;
		$d   = Owfax_Vitrine_Plugin::defaults();
		$int = function ( $k, $min, $max ) use ( $in, $d ) {
			return isset( $in[ $k ] ) ? max( $min, min( $max, (int) $in[ $k ] ) ) : $d[ $k ];
		};
		return array(
			'quota_mb'            => $int( 'quota_mb', 50, 1024 * 1024 ),
			'max_age_days'        => $int( 'max_age_days', 1, 3650 ),
			'max_count'           => $int( 'max_count', 1, 100000 ),
			'moderation'          => isset( $in['moderation'] ) && 'draft' === $in['moderation'] ? 'draft' : 'auto',
			'rate_per_hour'       => $int( 'rate_per_hour', 1, 1000 ),
			'delete_on_uninstall' => ! empty( $in['delete_on_uninstall'] ),
		);
	}

	public static function page(): void {
		if ( ! current_user_can( 'manage_options' ) ) {
			return;
		}
		$s    = Owfax_Vitrine_Plugin::settings();
		$st   = Owfax_Vitrine_Purge::stats();
		$name = Owfax_Vitrine_Plugin::SETTINGS;
		echo '<div class="wrap"><h1>' . esc_html__( 'Weather fax showcase', 'owfax-vitrine' ) . '</h1>';

		echo '<h2>' . esc_html__( 'Storage', 'owfax-vitrine' ) . '</h2><table class="widefat striped" style="max-width:640px"><tbody>';
		$rows = array(
			__( 'Space used', 'owfax-vitrine' )      => sprintf( '%s / %s (%s %%)', size_format( $st['used'] ), size_format( $st['quota'] ), $st['quota'] ? round( 100 * $st['used'] / $st['quota'] ) : 0 ),
			__( 'Charts', 'owfax-vitrine' )          => number_format_i18n( $st['count'] ),
			__( 'Charts to keep', 'owfax-vitrine' )  => sprintf( '%s / %s', size_format( $st['kept'] ), size_format( (int) ( Owfax_Vitrine_Rules::KEEP_SHARE * $st['quota'] ) ) ),
			__( 'Oldest chart', 'owfax-vitrine' )    => null === $st['oldest'] ? '—' : gmdate( 'Y-m-d H:i', $st['oldest'] ) . ' UTC',
			__( 'Quota reached', 'owfax-vitrine' )   => null === $st['days_to_quota'] ? __( 'nothing received in the last 7 days', 'owfax-vitrine' )
				/* translators: %s: number of days. */
				: sprintf( __( 'in %s days at the pace of the last 7 days, then the oldest charts are purged', 'owfax-vitrine' ), number_format_i18n( $st['days_to_quota'], 0 ) ),
		);
		foreach ( $rows as $k => $v ) {
			echo '<tr><th scope="row">' . esc_html( $k ) . '</th><td>' . esc_html( $v ) . '</td></tr>';
		}
		echo '</tbody></table>';

		echo '<form method="post" action="options.php">';
		settings_fields( self::PAGE );
		echo '<h2>' . esc_html__( 'Retention', 'owfax-vitrine' ) . '</h2><p>' .
			esc_html__( 'The first limit reached purges the oldest charts, with their sound and their page. The charts marked "to keep" are never purged.', 'owfax-vitrine' ) .
			'</p><table class="form-table" role="presentation">';
		$field = function ( $key, $label, $unit ) use ( $s, $name ) {
			echo '<tr><th scope="row"><label for="owfax-' . esc_attr( $key ) . '">' . esc_html( $label ) . '</label></th><td>' .
				'<input type="number" class="small-text" id="owfax-' . esc_attr( $key ) . '" name="' . esc_attr( $name . '[' . $key . ']' ) .
				'" value="' . esc_attr( (string) $s[ $key ] ) . '"> ' . esc_html( $unit ) . '</td></tr>';
		};
		$field( 'quota_mb', __( 'Disk quota', 'owfax-vitrine' ), __( 'MB', 'owfax-vitrine' ) );
		$field( 'max_age_days', __( 'Maximum age', 'owfax-vitrine' ), __( 'days', 'owfax-vitrine' ) );
		$field( 'max_count', __( 'Maximum number', 'owfax-vitrine' ), __( 'charts', 'owfax-vitrine' ) );
		echo '</table><h2>' . esc_html__( 'Publication', 'owfax-vitrine' ) . '</h2><table class="form-table" role="presentation">';
		echo '<tr><th scope="row">' . esc_html__( 'New charts', 'owfax-vitrine' ) . '</th><td><fieldset>';
		foreach ( array(
			'auto'  => __( 'published at once', 'owfax-vitrine' ),
			'draft' => __( 'saved as drafts, to publish by hand', 'owfax-vitrine' ),
		) as $v => $label ) {
			echo '<label><input type="radio" name="' . esc_attr( $name . '[moderation]' ) . '" value="' . esc_attr( $v ) . '"' .
				checked( $s['moderation'], $v, false ) . '> ' . esc_html( $label ) . '</label><br>';
		}
		echo '</fieldset></td></tr>';
		$field( 'rate_per_hour', __( 'Sends accepted per hour', 'owfax-vitrine' ), '' );
		echo '<tr><th scope="row">' . esc_html__( 'Uninstall', 'owfax-vitrine' ) . '</th><td><label><input type="checkbox" name="' .
			esc_attr( $name . '[delete_on_uninstall]' ) . '" value="1"' . checked( $s['delete_on_uninstall'], true, false ) . '> ' .
			esc_html__( 'delete the charts and their files when the plugin is uninstalled', 'owfax-vitrine' ) . '</label></td></tr></table>';
		submit_button();
		echo '</form>';

		echo '<h2>' . esc_html__( 'Station account', 'owfax-vitrine' ) . '</h2><ol>';
		echo '<li>' . esc_html__( 'Create a user with the role "owfax publisher": it can send charts, and nothing else.', 'owfax-vitrine' ) . '</li>';
		echo '<li>' . esc_html__( 'In its profile, create an application password, and give it to the station with the user name.', 'owfax-vitrine' ) . '</li>';
		/* translators: %s: endpoint URL. */
		echo '<li>' . sprintf( esc_html__( 'The station sends to %s, over HTTPS only.', 'owfax-vitrine' ), '<code>' . esc_html( rest_url( Owfax_Vitrine_Ingest::NS . '/charts' ) ) . '</code>' ) . '</li></ol>';
		echo '<p>' . esc_html__( 'On nginx, the uploads/owfax/.htaccess file is ignored: forbid scripts in that directory and add the cache headers by hand (see readme.txt).', 'owfax-vitrine' ) . '</p>';

		$refused = array_reverse( (array) get_option( Owfax_Vitrine_Plugin::REFUSALS, array() ) );
		echo '<h2>' . esc_html__( 'Refused sends', 'owfax-vitrine' ) . '</h2>';
		if ( ! $refused ) {
			echo '<p>' . esc_html__( 'None.', 'owfax-vitrine' ) . '</p>';
		} else {
			echo '<table class="widefat striped"><thead><tr><th>' . esc_html__( 'Time (UTC)', 'owfax-vitrine' ) . '</th><th>' .
				esc_html__( 'Chart', 'owfax-vitrine' ) . '</th><th>' . esc_html__( 'Reason', 'owfax-vitrine' ) . '</th></tr></thead><tbody>';
			foreach ( array_slice( $refused, 0, 20 ) as $r ) {
				echo '<tr><td>' . esc_html( gmdate( 'Y-m-d H:i:s', (int) $r[0] ) ) . '</td><td>' . esc_html( (string) $r[3] ) .
					'</td><td><code>' . esc_html( (string) $r[1] ) . '</code> ' . esc_html( (string) $r[2] ) . '</td></tr>';
			}
			echo '</tbody></table>';
		}
		echo '</div>';
	}
}
