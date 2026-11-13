<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Tests of the WordPress plugin rules that need no WordPress: schema validation,
// real file types, purge plan, keep cap, projection (docs/VITRINE-WEB.md WPP-001,
// WPP-010 … WPP-014). Run: php integrations/wordpress/tests/test-rules.php

define( 'OWFAX_VITRINE_STANDALONE', true );
$plugin = dirname( __DIR__ ) . '/owfax-vitrine';
require $plugin . '/includes/class-schema.php';
require $plugin . '/includes/class-rules.php';

$failures = 0;
function check( bool $ok, string $what ): void {
	global $failures;
	if ( ! $ok ) {
		++$failures;
		echo "  FAILED: $what\n";
	}
}
function test( string $name, callable $fn ): void {
	global $failures;
	$before = $failures;
	$fn();
	echo ( $failures === $before ? 'PASS ' : 'FAIL ' ) . $name . "\n";
}

$schema = Owfax_Vitrine_Schema::from_file( $plugin . '/schema/owfax-vitrine-0.1.schema.json' );

/** A valid meta.json, as owfax-vitrine writes it. */
function meta(): stdClass {
	return json_decode(
		'{"schema": "owfax-vitrine/0.1", "chart_id": "20261110T1007Z_DDK6_13882.5kHz", "replay": true,
		"software": {"name": "openWeatherFaxSSB", "version": "0.1.0", "commit": "09e4cbd", "source_url": "https://github.com/Bob56fr/openWeatherFaxSSB"},
		"station": {"id": "dwd-pinneberg", "name": "Deutscher Wetterdienst — Hamburg/Pinneberg", "callsign": "DDK6", "assigned_khz": 13882.5, "power_kw": 20.0, "attribution": "Datenbasis: Deutscher Wetterdienst", "terms_url": "https://www.dwd.de/copyright"},
		"receiver": {"locator": "IN87hr", "rig": "Icom IC-7300MK2", "input": "usb-audio", "dial_khz": 13880.6, "mode": "PKTUSB"},
		"path": {"distance_km": 1137, "distance_nm": 614, "azimuth_deg": 50, "daylight": "day"},
		"time": {"start_utc": "2026-11-10T10:07:24Z", "end_utc": "2026-11-10T10:18:32Z", "duration_s": 668.0},
		"wefax": {"lpm": 120, "ioc": 576, "lines": 1295, "width_px": 1810, "black_hz": 1468, "white_hz": 2318, "clock_ppm": -8.0, "tuning_offset_hz": -6.7, "apt_start": true, "phasing": true, "apt_stop": true},
		"quality": {"mean": 0.991, "snr_db_median": 25.8, "lines_bad_fraction": 0.0},
		"diversity": {"enabled": false, "sources": []},
		"disturbances": {"detector": "audio-v1", "local_fraction": 0.0, "fading_fraction": 0.0, "zones": []},
		"sync": {"audio_duration_s": 696.344, "apt_start_s": 2.0, "phasing_s": 7.0, "image_s": 36.847, "image_end_s": 684.344, "line_period_s": 0.499998},
		"treatments": {"raw": [], "enhanced": ["clicks"]},
		"selection": {"publish": true, "reasons": []},
		"files": {"raw": "raw.webp", "enhanced": "enh.webp", "thumb": "thumb.webp", "audio": "audio.m4a", "telemetry": "telemetry.json"}}'
	);
}

test(
	'a package written by the station is valid',
	function () use ( $schema ) {
		$errors = Owfax_Vitrine_Rules::check_meta( meta(), $schema );
		check( array() === $errors, 'valid meta: ' . implode( '; ', $errors ) );
		check( 1794305244 === Owfax_Vitrine_Rules::start_unix( meta() ), 'start time' );
	}
);

test(
	'the schema refuses what it must (WPP-001)',
	function () use ( $schema ) {
		$cases = array(
			'missing station'      => function ( $m ) { unset( $m->station ); },
			'unknown field'        => function ( $m ) { $m->extra = 1; },
			'path in chart id'     => function ( $m ) { $m->chart_id = '../../wp-config_X_1kHz'; },
			'not a replay'         => function ( $m ) { $m->replay = false; },
			'wrong schema'         => function ( $m ) { $m->schema = 'owfax-vitrine/9'; },
			'string for a number'  => function ( $m ) { $m->wefax->lines = '1295'; },
			'object for an array'  => function ( $m ) { $m->diversity->sources = new stdClass(); },
			'no attribution'       => function ( $m ) { $m->station->attribution = ''; },
			'unknown cause'        => function ( $m ) {
				$m->disturbances->zones = array( (object) array( 'start_s' => 1, 'end_s' => 20, 'first_line' => 0, 'last_line' => 38, 'cause' => 'aliens', 'local' => true, 'muted' => true, 'indicators' => array() ) );
			},
		);
		foreach ( $cases as $name => $spoil ) {
			$m = meta();
			$spoil( $m );
			check( array() !== Owfax_Vitrine_Rules::check_meta( $m, $schema ), $name . ' accepted' );
		}
	}
);

test(
	'beyond the schema: not selected, bad time, image ending before it starts',
	function () use ( $schema ) {
		$m                     = meta();
		$m->selection->publish = false;
		$m->selection->reasons = array( 'local_disturbance' );
		check( array() !== Owfax_Vitrine_Rules::check_meta( $m, $schema ), 'unselected chart accepted' );
		$m                  = meta();
		$m->time->start_utc = '2026-11-10 10:07';
		check( array() !== Owfax_Vitrine_Rules::check_meta( $m, $schema ), 'bad time accepted' );
		$m                    = meta();
		$m->sync->image_end_s = 1.0;
		check( array() !== Owfax_Vitrine_Rules::check_meta( $m, $schema ), 'image ending first accepted' );
		check( array() !== Owfax_Vitrine_Rules::check_meta( 'text', $schema ), 'non-object accepted' );
	}
);

test(
	'real file types, whatever the name (WPP-001)',
	function () {
		$dir  = sys_get_temp_dir();
		$f    = $dir . '/owfax-sniff-' . getmypid();
		$cases = array(
			array( "RIFF\x10\0\0\0WEBPVP8 ", 'webp', true ),
			array( "RIFF\x10\0\0\0WAVEfmt ", 'webp', false ),
			array( "\0\0\0\x20ftypM4A \0\0\0\0", 'm4a', true ),
			array( '<?php echo 1; ?>', 'm4a', false ),
			array( '<?php echo 1; ?>', 'webp', false ),
			array( '{"a": 1}', 'json', true ),
			array( '[1, 2]', 'json', false ),
			array( '{"a": ', 'json', false ),
		);
		foreach ( $cases as $c ) {
			file_put_contents( $f, $c[0] );
			check( Owfax_Vitrine_Rules::sniff( $f, $c[1] ) === $c[2], $c[1] . ' sniff of ' . json_encode( substr( $c[0], 0, 8 ) ) );
		}
		@unlink( $f );
	}
);

test(
	'telemetry.json is checked',
	function () {
		$ok = json_decode( '{"schema": "owfax-vitrine-telemetry/0.1", "interval_s": 1, "audio_dbfs": [], "snr_db": [], "line_quality": []}' );
		check( array() === Owfax_Vitrine_Rules::check_telemetry( $ok ), 'valid telemetry' );
		unset( $ok->line_quality );
		check( array() !== Owfax_Vitrine_Rules::check_telemetry( $ok ), 'missing line_quality accepted' );
		check( array() !== Owfax_Vitrine_Rules::check_telemetry( json_decode( '{"schema": "x"}' ) ), 'wrong schema accepted' );
	}
);

$mb  = 1024 * 1024;
$now = 1_800_000_000;
$day = 86400;
/** Charts of 5 MB, one per day, the oldest first; ids 1 … n. */
function charts( int $n, int $now, array $keep = array() ): array {
	$items = array();
	for ( $i = 1; $i <= $n; ++$i ) {
		$items[] = array(
			'id'    => $i,
			'start' => $now - ( $n - $i + 1 ) * 86400 + 3600,
			'bytes' => 5 * 1024 * 1024,
			'keep'  => in_array( $i, $keep, true ),
		);
	}
	return $items;
}

test(
	'the purge respects the quota, the oldest first (WPP-010, WPP-011)',
	function () use ( $mb, $now ) {
		$limits = array( 'quota_bytes' => 50 * $mb, 'max_age_days' => 60, 'max_count' => 300 );
		// 10 charts = 50 MB: a new one of 5 MB needs one to go, the oldest.
		$p = Owfax_Vitrine_Rules::plan_purge( charts( 10, $now ), 5 * $mb, $limits, $now );
		check( array( 1 ) === $p['delete'] && $p['fits'], 'oldest first: ' . json_encode( $p ) );
		// A bigger one needs two.
		$p = Owfax_Vitrine_Rules::plan_purge( charts( 10, $now ), 8 * $mb, $limits, $now );
		check( array( 1, 2 ) === $p['delete'] && $p['fits'], 'two for 8 MB: ' . json_encode( $p ) );
		// Room left: nothing goes.
		$p = Owfax_Vitrine_Rules::plan_purge( charts( 9, $now ), 5 * $mb, $limits, $now );
		check( array() === $p['delete'] && $p['fits'], 'room left' );
		// The charts to keep are skipped, the next oldest goes.
		$p = Owfax_Vitrine_Rules::plan_purge( charts( 10, $now, array( 1, 2 ) ), 5 * $mb, $limits, $now );
		check( array( 3 ) === $p['delete'] && $p['fits'], 'keep skipped: ' . json_encode( $p ) );
	}
);

test(
	'507 when only charts to keep are left (WPP-014)',
	function () use ( $mb, $now ) {
		$limits = array( 'quota_bytes' => 10 * $mb, 'max_age_days' => 60, 'max_count' => 300 );
		$p      = Owfax_Vitrine_Rules::plan_purge( charts( 2, $now, array( 1, 2 ) ), 5 * $mb, $limits, $now );
		check( ! $p['fits'] && array() === $p['delete'], 'kept charts cannot make room' );
		$p = Owfax_Vitrine_Rules::plan_purge( array(), 11 * $mb, $limits, $now );
		check( ! $p['fits'], 'a package bigger than the quota does not fit' );
	}
);

test(
	'maximum age and maximum number (WPP-010, WPP-012)',
	function () use ( $mb, $now ) {
		$limits = array( 'quota_bytes' => 2048 * $mb, 'max_age_days' => 5, 'max_count' => 300 );
		// Daily run (nothing incoming): charts older than 5 days go, kept ones stay.
		$p = Owfax_Vitrine_Rules::plan_purge( charts( 8, $now, array( 1 ) ), 0, $limits, $now );
		check( array( 2, 3 ) === $p['delete'], 'age: ' . json_encode( $p ) );
		$limits = array( 'quota_bytes' => 2048 * $mb, 'max_age_days' => 60, 'max_count' => 5 );
		$p      = Owfax_Vitrine_Rules::plan_purge( charts( 5, $now ), 5 * $mb, $limits, $now );
		check( array( 1 ) === $p['delete'] && $p['fits'], 'count: ' . json_encode( $p ) );
	}
);

test(
	'charts to keep: at most 20 % of the quota (WPP-011)',
	function () use ( $mb, $now ) {
		$items = charts( 10, $now, array( 1 ) );  // 5 MB kept
		check( Owfax_Vitrine_Rules::keep_allowed( $items, 5 * $mb, 50 * $mb ), '10 MB of 50 allowed' );
		check( ! Owfax_Vitrine_Rules::keep_allowed( $items, 6 * $mb, 50 * $mb ), '11 MB of 50 refused' );
	}
);

test(
	'projection of the quota on the last 7 days (WPP-013)',
	function () use ( $mb, $now, $day ) {
		$log = array();
		for ( $d = 1; $d <= 7; ++$d ) {
			$log[] = array( $now - $d * $day + 60, 70 * $mb );  // 70 MB a day
		}
		$log[] = array( $now - 30 * $day, 999 * $mb );        // too old to count
		$days  = Owfax_Vitrine_Rules::days_to_quota( $log, 1348 * $mb, 2048 * $mb, $now );
		check( null !== $days && abs( $days - 10.0 ) < 1e-9, 'ten days left: ' . var_export( $days, true ) );
		check( null === Owfax_Vitrine_Rules::days_to_quota( array(), 0, 2048 * $mb, $now ), 'nothing received' );
	}
);

echo $failures ? "$failures failed\n" : "all passed\n";
exit( $failures ? 1 : 0 );
