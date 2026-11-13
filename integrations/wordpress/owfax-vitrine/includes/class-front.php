<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Public side (WPP-020 … WPP-022, WPP-030, WPP-041): shortcodes and blocks, the page
// of each chart, Open Graph tags. The replay player is the one of the station
// dashboard (assets/owfax-player.js, a copy of web/vitrine/): once a page is shown,
// it only reads static files from uploads/owfax/ (VIT-002).
//
//   [owfax_player playlist="10" rate="10" station="dwd-pinneberg"]
//   [owfax_gallery limit="24" station="…"]
//   [owfax_chart id="20261110T1007Z_DDK6_13882.5kHz"]
//   [owfax_stations]

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Front {
	public static function init(): void {
		add_action( 'init', array( __CLASS__, 'register' ) );
		add_action( 'wp_enqueue_scripts', array( __CLASS__, 'enqueue_single' ) );
		add_filter( 'the_content', array( __CLASS__, 'single_content' ) );
		add_action( 'wp_head', array( __CLASS__, 'open_graph' ), 5 );
	}

	public static function register(): void {
		$url = plugins_url( 'assets/', OWFAX_VITRINE_FILE );
		$v   = OWFAX_VITRINE_VERSION;
		wp_register_style( 'owfax-player', $url . 'owfax-player.css', array(), $v );
		wp_register_style( 'owfax-vitrine', $url . 'owfax-vitrine.css', array( 'owfax-player' ), $v );
		wp_register_script( 'owfax-player', $url . 'owfax-player.js', array(), $v, true );
		wp_register_script( 'owfax-vitrine', $url . 'owfax-vitrine.js', array( 'owfax-player' ), $v, true );
		wp_register_script(
			'owfax-vitrine-blocks',
			$url . 'blocks.js',
			array( 'wp-blocks', 'wp-element', 'wp-components', 'wp-block-editor', 'wp-server-side-render', 'wp-i18n' ),
			$v,
			true
		);
		if ( function_exists( 'wp_set_script_translations' ) ) {
			wp_set_script_translations( 'owfax-vitrine-blocks', 'owfax-vitrine', OWFAX_VITRINE_DIR . 'languages' );
		}

		add_shortcode( 'owfax_player', array( __CLASS__, 'player_shortcode' ) );
		add_shortcode( 'owfax_gallery', array( __CLASS__, 'gallery_shortcode' ) );
		add_shortcode( 'owfax_chart', array( __CLASS__, 'chart_shortcode' ) );
		add_shortcode( 'owfax_stations', array( __CLASS__, 'stations_shortcode' ) );

		$blocks = array(
			'owfax/player'  => array(
				'cb'    => 'player_shortcode',
				'attrs' => array(
					'playlist' => array( 'type' => 'number', 'default' => 1 ),
					'rate'     => array( 'type' => 'number', 'default' => 1 ),
					'station'  => array( 'type' => 'string', 'default' => '' ),
				),
			),
			'owfax/gallery' => array(
				'cb'    => 'gallery_shortcode',
				'attrs' => array(
					'limit'   => array( 'type' => 'number', 'default' => 24 ),
					'station' => array( 'type' => 'string', 'default' => '' ),
				),
			),
			'owfax/chart'   => array(
				'cb'    => 'chart_shortcode',
				'attrs' => array( 'id' => array( 'type' => 'string', 'default' => '' ) ),
			),
		);
		foreach ( $blocks as $name => $b ) {
			register_block_type(
				$name,
				array(
					'api_version'     => 2,
					'editor_script'   => 'owfax-vitrine-blocks',
					'attributes'      => $b['attrs'],
					'render_callback' => array( __CLASS__, $b['cb'] ),
				)
			);
		}
	}

	/** Scripts and styles only where a player or a gallery is (WPP-041). */
	private static function enqueue(): void {
		wp_enqueue_style( 'owfax-vitrine' );
		wp_enqueue_script( 'owfax-vitrine' );
	}

	public static function enqueue_single(): void {
		if ( is_singular( Owfax_Vitrine_Charts::TYPE ) ) {
			self::enqueue();
		}
	}

	private static function lang(): string {
		return 0 === strpos( determine_locale(), 'fr' ) ? 'fr' : 'en';
	}

	/** Published charts, newest first. */
	private static function latest( int $n, string $station = '' ): array {
		$args = array(
			'post_type'      => Owfax_Vitrine_Charts::TYPE,
			'post_status'    => 'publish',
			'posts_per_page' => max( 1, min( 100, $n ) ),
			'meta_key'       => Owfax_Vitrine_Charts::META_START,
			'orderby'        => 'meta_value_num',
			'order'          => 'DESC',
			'no_found_rows'  => true,
		);
		if ( '' !== $station ) {
			$args['meta_query'] = array(
				array(
					'key'   => Owfax_Vitrine_Charts::META_STATION,
					'value' => $station,
				),
			);
		}
		return ( new WP_Query( $args ) )->posts;
	}

	/** The player element; its fallback (no JavaScript) is the thumbnail and a link. */
	private static function player_html( array $posts, int $rate, bool $auto ): string {
		if ( ! $posts ) {
			return '<p class="owfax-empty">' . esc_html__( 'No chart published yet.', 'owfax-vitrine' ) . '</p>';
		}
		self::enqueue();
		$bases = array();
		foreach ( $posts as $p ) {
			$bases[] = Owfax_Vitrine_Store::chart_url( (string) get_post_meta( $p->ID, Owfax_Vitrine_Charts::META_ID, true ) );
		}
		$first = $posts[0];
		$id    = (string) get_post_meta( $first->ID, Owfax_Vitrine_Charts::META_ID, true );
		$html  = '<div class="owfax-player" data-bases="' . esc_attr( wp_json_encode( $bases ) ) . '"' .
			' data-lang="' . esc_attr( self::lang() ) . '" data-rate="' . esc_attr( (string) $rate ) . '"' .
			' data-auto="' . ( $auto ? '1' : '0' ) . '">';
		$html .= '<p><a href="' . esc_url( get_permalink( $first ) ) . '"><img src="' .
			esc_url( Owfax_Vitrine_Store::chart_url( $id ) . 'thumb.webp' ) . '" alt="' .
			esc_attr( get_the_title( $first ) ) . '" loading="lazy"></a></p>';
		$html .= '<p class="owfax-replay"><strong>' . esc_html__( 'Replay', 'owfax-vitrine' ) . '</strong> — ' .
			esc_html( get_the_title( $first ) ) . '</p>';
		$html .= '<p class="owfax-warning">' . esc_html( self::warning() ) . '</p>';
		return $html . '</div>';
	}

	/** VIT-004, also on pages without JavaScript. */
	private static function warning(): string {
		return __( 'Demonstration — not for navigation, use the official bulletins.', 'owfax-vitrine' );
	}

	private static function rate( $v ): int {
		$r = (int) $v;
		return in_array( $r, array( 1, 4, 10 ), true ) ? $r : 1;
	}

	public static function player_shortcode( $atts ): string {
		$a = shortcode_atts(
			array(
				'playlist' => 1,
				'rate'     => 1,
				'station'  => '',
			),
			(array) $atts,
			'owfax_player'
		);
		$n = max( 1, (int) $a['playlist'] );
		// A playlist starts by itself when it comes into view, muted (PLY-007, PLY-021).
		return self::player_html( self::latest( $n, sanitize_key( $a['station'] ) ), self::rate( $a['rate'] ), $n > 1 );
	}

	public static function chart_shortcode( $atts ): string {
		$a  = shortcode_atts( array( 'id' => '' ), (array) $atts, 'owfax_chart' );
		$id = (string) $a['id'];
		if ( ! Owfax_Vitrine_Plugin::is_chart_id( $id ) ) {
			return '';
		}
		$post = Owfax_Vitrine_Charts::find( $id );
		if ( ! $post || 'publish' !== $post->post_status ) {
			return '';
		}
		return self::player_html( array( $post ), 1, false );
	}

	public static function gallery_shortcode( $atts ): string {
		$a       = shortcode_atts(
			array(
				'limit'   => 24,
				'station' => '',
			),
			(array) $atts,
			'owfax_gallery'
		);
		$station = sanitize_key( $a['station'] );
		// Filter by station from the page itself (WPP-020): ?owfax_station=…
		if ( '' === $station && isset( $_GET['owfax_station'] ) ) {
			$station = sanitize_key( wp_unslash( $_GET['owfax_station'] ) );
		}
		$posts = self::latest( (int) $a['limit'], $station );
		self::enqueue();
		$out = '';
		if ( '' === $a['station'] ) {
			$out .= '<nav class="owfax-filter" aria-label="' . esc_attr__( 'Stations', 'owfax-vitrine' ) . '">';
			$out .= '<a href="' . esc_url( remove_query_arg( 'owfax_station' ) ) . '"' .
				( '' === $station ? ' aria-current="true"' : '' ) . '>' . esc_html__( 'All stations', 'owfax-vitrine' ) . '</a>';
			foreach ( self::stations() as $s ) {
				$out .= '<a href="' . esc_url( add_query_arg( 'owfax_station', $s['id'] ) ) . '"' .
					( $s['id'] === $station ? ' aria-current="true"' : '' ) . '>' . esc_html( $s['name'] ) . '</a>';
			}
			$out .= '</nav>';
		}
		if ( ! $posts ) {
			return $out . '<p class="owfax-empty">' . esc_html__( 'No chart published yet.', 'owfax-vitrine' ) . '</p>';
		}
		$out .= '<ul class="owfax-gallery">';
		foreach ( $posts as $p ) {
			$m = Owfax_Vitrine_Charts::meta( $p->ID );
			if ( ! $m ) {
				continue;
			}
			$q     = (int) round( 100 * $m->quality->mean );
			$grade = $q >= 90 ? 'good' : ( $q >= 75 ? 'fair' : 'poor' );
			$out  .= '<li><a href="' . esc_url( get_permalink( $p ) ) . '">';
			$out  .= '<img src="' . esc_url( Owfax_Vitrine_Store::chart_url( $m->chart_id ) . 'thumb.webp' ) .
				'" alt="" loading="lazy" width="400">';
			$out  .= '<span class="owfax-caption"><span class="owfax-when">' .
				esc_html( gmdate( 'd/m/Y H:i', (int) Owfax_Vitrine_Rules::start_unix( $m ) ) . ' UTC' ) . '</span>';
			$out  .= '<span class="owfax-station">' . esc_html( $m->station->callsign . ' · ' . self::khz( $m ) ) . '</span>';
			$out  .= '<span class="owfax-facts">' . esc_html( $m->station->name ) . ' · ' .
				esc_html( number_format_i18n( $m->path->distance_km ) . ' km' ) . '</span>';
			/* translators: %d: mean line quality in percent. */
			$out .= '<span class="owfax-quality owfax-' . $grade . '">' . esc_html( sprintf( __( 'Quality %d %%', 'owfax-vitrine' ), $q ) ) . '</span>';
			$out .= '</span></a></li>';
		}
		return $out . '</ul>';
	}

	private static function khz( $m ): string {
		$k = (float) $m->station->assigned_khz;
		return number_format_i18n( $k, fmod( $k, 1.0 ) > 0 ? 1 : 0 ) . ' kHz';
	}

	/** Sources received: distance, charts published, best quality (WPP-030). */
	private static function stations(): array {
		$posts = get_posts(
			array(
				'post_type'      => Owfax_Vitrine_Charts::TYPE,
				'post_status'    => 'publish',
				'posts_per_page' => -1,
				'no_found_rows'  => true,
			)
		);
		$out   = array();
		foreach ( $posts as $p ) {
			$m = Owfax_Vitrine_Charts::meta( $p->ID );
			if ( ! $m ) {
				continue;
			}
			$id = $m->station->id;
			if ( ! isset( $out[ $id ] ) ) {
				$out[ $id ] = array(
					'id'       => $id,
					'name'     => $m->station->name,
					'callsign' => array(),
					'km'       => $m->path->distance_km,
					'nm'       => $m->path->distance_nm,
					'count'    => 0,
					'best'     => 0.0,
				);
			}
			$out[ $id ]['count']++;
			$out[ $id ]['best']                               = max( $out[ $id ]['best'], (float) $m->quality->mean );
			$out[ $id ]['callsign'][ $m->station->callsign ] = true;
		}
		uasort(
			$out,
			function ( $a, $b ) {
				return $a['km'] <=> $b['km'];
			}
		);
		return array_values( $out );
	}

	public static function stations_shortcode(): string {
		$rows = self::stations();
		if ( ! $rows ) {
			return '<p class="owfax-empty">' . esc_html__( 'No chart published yet.', 'owfax-vitrine' ) . '</p>';
		}
		self::enqueue();
		$out  = '<table class="owfax-stations"><thead><tr><th>' . esc_html__( 'Station', 'owfax-vitrine' ) . '</th><th>' .
			esc_html__( 'Distance', 'owfax-vitrine' ) . '</th><th>' . esc_html__( 'Charts', 'owfax-vitrine' ) . '</th><th>' .
			esc_html__( 'Best quality', 'owfax-vitrine' ) . '</th></tr></thead><tbody>';
		foreach ( $rows as $r ) {
			$out .= '<tr><td>' . esc_html( $r['name'] . ' (' . implode( ', ', array_keys( $r['callsign'] ) ) . ')' ) . '</td><td>' .
				esc_html( number_format_i18n( $r['km'] ) . ' km (' . number_format_i18n( $r['nm'] ) . ' NM)' ) . '</td><td>' .
				esc_html( number_format_i18n( $r['count'] ) ) . '</td><td>' . esc_html( round( 100 * $r['best'] ) . ' %' ) . '</td></tr>';
		}
		return $out . '</tbody></table>';
	}

	// ------------------------------------------------------- page of a chart

	/** Player and full metadata on the page of a chart (WPP-021, WPP-022). */
	public static function single_content( $content ) {
		if ( ! is_singular( Owfax_Vitrine_Charts::TYPE ) || ! in_the_loop() || ! is_main_query() ) {
			return $content;
		}
		$post = get_post();
		$m    = $post ? Owfax_Vitrine_Charts::meta( $post->ID ) : null;
		if ( ! $m ) {
			return $content;
		}
		$rows = array();
		$st   = $m->station;
		$add  = function ( $label, $value ) use ( &$rows ) {
			if ( '' !== (string) $value ) {
				$rows[] = '<dt>' . esc_html( $label ) . '</dt><dd>' . esc_html( $value ) . '</dd>';
			}
		};
		$add( __( 'Station', 'owfax-vitrine' ), $st->name . ' (' . $st->callsign . ')' );
		$add( __( 'Frequency', 'owfax-vitrine' ), self::khz( $m ) . ( $st->power_kw ? ' — ' . number_format_i18n( $st->power_kw ) . ' kW' : '' ) );
		$daylight = array(
			'day'   => __( 'daylight path', 'owfax-vitrine' ),
			'night' => __( 'night path', 'owfax-vitrine' ),
			'mixed' => __( 'day and night path', 'owfax-vitrine' ),
		);
		/* translators: 1: km, 2: nautical miles, 3: bearing in degrees. */
		$add( __( 'Distance', 'owfax-vitrine' ), sprintf( __( '%1$s km (%2$s NM), bearing %3$s°', 'owfax-vitrine' ), number_format_i18n( $m->path->distance_km ), number_format_i18n( $m->path->distance_nm ), number_format_i18n( $m->path->azimuth_deg ) ) . ', ' . ( isset( $daylight[ $m->path->daylight ] ) ? $daylight[ $m->path->daylight ] : $m->path->daylight ) );
		$add( __( 'Received (UTC)', 'owfax-vitrine' ), str_replace( array( 'T', 'Z' ), array( ' ', '' ), $m->time->start_utc ) . ' → ' . substr( $m->time->end_utc, 11, 8 ) );
		/* translators: 1: number of lines, 2: lines per minute, 3: index of cooperation. */
		$add( __( 'Chart', 'owfax-vitrine' ), sprintf( __( '%1$s lines, %2$s lpm, IOC %3$s', 'owfax-vitrine' ), number_format_i18n( $m->wefax->lines ), $m->wefax->lpm, $m->wefax->ioc ) );
		/* translators: 1: mean line quality in percent, 2: median SNR in dB. */
		$add( __( 'Quality', 'owfax-vitrine' ), sprintf( __( '%1$s %% of the lines, median SNR %2$s dB', 'owfax-vitrine' ), round( 100 * $m->quality->mean ), number_format_i18n( $m->quality->snr_db_median, 1 ) ) );
		$add( __( 'Receiver', 'owfax-vitrine' ), $m->receiver->rig . ' — ' . $m->receiver->locator );
		$add( __( 'Diversity', 'owfax-vitrine' ), $m->diversity->enabled ? __( 'yes', 'owfax-vitrine' ) : __( 'no', 'owfax-vitrine' ) );
		$names = array( 'clicks' => __( 'click removal', 'owfax-vitrine' ) );
		$done  = array();
		foreach ( $m->treatments->enhanced as $t ) {
			$done[] = isset( $names[ $t ] ) ? $names[ $t ] : $t;
		}
		$add( __( 'Enhanced image', 'owfax-vitrine' ), $done ? implode( ', ', $done ) : __( 'no treatment', 'owfax-vitrine' ) );
		if ( $m->disturbances->local_fraction > 0 ) {
			/* translators: %s: share of the lines in percent. */
			$add( __( 'Local disturbance', 'owfax-vitrine' ), sprintf( __( '%s %% of the lines, sound muted there', 'owfax-vitrine' ), round( 100 * $m->disturbances->local_fraction ) ) );
		}

		$html  = self::player_html( array( $post ), 1, false );
		$html .= '<dl class="owfax-details">' . implode( '', $rows ) . '</dl>';
		$html .= '<p class="owfax-attribution">' . esc_html( $st->attribution );
		if ( preg_match( '#^https?://#', $st->terms_url ) ) {
			$html .= ' — <a href="' . esc_url( $st->terms_url ) . '" rel="noopener">' . esc_html__( 'Terms of use', 'owfax-vitrine' ) . '</a>';
		}
		$html .= '</p>';
		$sw    = $m->software;
		/* translators: 1: software name and version, 2: commit. */
		$html .= '<p class="owfax-software">' . sprintf(
			esc_html__( 'Received and decoded by %1$s (%2$s), free software.', 'owfax-vitrine' ),
			'<a href="' . esc_url( $sw->source_url ) . '">' . esc_html( $sw->name . ' ' . $sw->version ) . '</a>',
			esc_html( $sw->commit )
		) . '</p>';
		return $content . $html;
	}

	public static function open_graph(): void {
		if ( ! is_singular( Owfax_Vitrine_Charts::TYPE ) ) {
			return;
		}
		$post = get_queried_object();
		$m    = $post instanceof WP_Post ? Owfax_Vitrine_Charts::meta( $post->ID ) : null;
		if ( ! $m ) {
			return;
		}
		$title = get_the_title( $post );
		/* translators: 1: distance in km, 2: mean line quality in percent. */
		$desc  = __( 'Replay', 'owfax-vitrine' ) . ' — ' . sprintf( __( 'weather fax received %1$s km away, %2$s %% of the lines good.', 'owfax-vitrine' ), number_format_i18n( $m->path->distance_km ), round( 100 * $m->quality->mean ) ) . ' ' . self::warning();
		$image = Owfax_Vitrine_Store::chart_url( $m->chart_id ) . 'thumb.webp';
		$tags  = array(
			'og:type'             => 'article',
			'og:title'            => $title,
			'og:description'      => $desc,
			'og:url'              => get_permalink( $post ),
			'og:image'            => $image,
			'og:image:alt'        => $title,
			'twitter:card'        => 'summary_large_image',
			'twitter:title'       => $title,
			'twitter:description' => $desc,
			'twitter:image'       => $image,
		);
		foreach ( $tags as $k => $v ) {
			$attr = 0 === strpos( $k, 'og:' ) ? 'property' : 'name';
			echo '<meta ' . $attr . '="' . esc_attr( $k ) . '" content="' . esc_attr( $v ) . '">' . "\n";
		}
	}
}
