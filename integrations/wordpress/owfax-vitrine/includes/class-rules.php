<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// The rules of the showcase that need no WordPress: which files a chart package
// holds and how big they may be, how to recognise their real type, how to check
// meta.json and telemetry.json, and which charts the purge removes
// (docs/VITRINE-WEB.md WPP-001, WPP-010 … WPP-014). Tested with plain PHP.

defined( 'OWFAX_VITRINE_STANDALONE' ) || defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Rules {
	const TELEMETRY_SCHEMA = 'owfax-vitrine-telemetry/0.1';
	const KEEP_SHARE       = 0.20;  // "to keep" charts: at most 20 % of the quota (WPP-011)

	/**
	 * Files of a package, by multipart field: published name, maximum size, kind,
	 * required. Sizes leave room above the measured 3 to 7 MB per chart.
	 *
	 * @return array<string, array{0: string, 1: int, 2: string, 3: bool}>
	 */
	public static function files(): array {
		return array(
			'meta'      => array( 'meta.json', 64 * 1024, 'json', true ),
			'telemetry' => array( 'telemetry.json', 1024 * 1024, 'json', true ),
			'raw'       => array( 'raw.webp', 8 * 1024 * 1024, 'webp', true ),
			'enhanced'  => array( 'enh.webp', 8 * 1024 * 1024, 'webp', false ),
			'thumb'     => array( 'thumb.webp', 512 * 1024, 'webp', true ),
			'audio'     => array( 'audio.m4a', 16 * 1024 * 1024, 'm4a', true ),
		);
	}

	/** True if the file really is of `kind` (magic bytes, not the name or the client's type). */
	public static function sniff( string $path, string $kind ): bool {
		$head = (string) @file_get_contents( $path, false, null, 0, 16 );
		switch ( $kind ) {
			case 'webp':
				return strlen( $head ) >= 12 && 'RIFF' === substr( $head, 0, 4 ) &&
					'WEBP' === substr( $head, 8, 4 );
			case 'm4a':
				// ISO base media file: a 'ftyp' box first (AAC in MP4, PUB-030).
				return strlen( $head ) >= 12 && 'ftyp' === substr( $head, 4, 4 );
			case 'json':
				$text = (string) @file_get_contents( $path );
				return is_object( json_decode( $text ) );
		}
		return false;
	}

	/**
	 * Checks meta.json against the published schema, plus what the schema cannot say.
	 *
	 * @param object $meta Decoded as objects.
	 * @return string[] Errors, empty if the package can be published.
	 */
	public static function check_meta( $meta, Owfax_Vitrine_Schema $schema ): array {
		if ( ! is_object( $meta ) ) {
			return array( 'meta.json: not a JSON object' );
		}
		$errors = array();
		foreach ( $schema->validate( $meta ) as $e ) {
			$errors[] = 'meta.json ' . $e;
		}
		if ( $errors ) {
			return $errors;
		}
		if ( '' === trim( $meta->station->attribution ) ) {
			// The weather service's attribution is shown with every chart (PUB-024).
			$errors[] = 'meta.json /station/attribution: empty';
		}
		if ( true !== $meta->selection->publish ) {
			$errors[] = 'meta.json: the station did not select this chart for publication';
		}
		if ( null === self::start_unix( $meta ) ) {
			$errors[] = 'meta.json /time/start_utc: not a UTC time (YYYY-MM-DDTHH:MM:SSZ)';
		}
		if ( $meta->sync->image_end_s <= $meta->sync->image_s ) {
			$errors[] = 'meta.json /sync: the image ends before it starts';
		}
		return $errors;
	}

	/** @param object $t telemetry.json, decoded as objects. */
	public static function check_telemetry( $t ): array {
		if ( ! is_object( $t ) || ! isset( $t->schema ) || self::TELEMETRY_SCHEMA !== $t->schema ) {
			return array( 'telemetry.json: schema must be ' . self::TELEMETRY_SCHEMA );
		}
		if ( ! isset( $t->interval_s ) || ! is_numeric( $t->interval_s ) || $t->interval_s <= 0 ) {
			return array( 'telemetry.json: interval_s missing' );
		}
		foreach ( array( 'audio_dbfs', 'snr_db', 'line_quality' ) as $k ) {
			if ( ! isset( $t->{$k} ) || ! is_array( $t->{$k} ) ) {
				return array( 'telemetry.json: ' . $k . ' missing' );
			}
		}
		return array();
	}

	/** Unix time of the chart start, null if start_utc is not "YYYY-MM-DDTHH:MM:SSZ". */
	public static function start_unix( $meta ): ?int {
		$s = isset( $meta->time->start_utc ) ? (string) $meta->time->start_utc : '';
		$d = DateTime::createFromFormat( '!Y-m-d\TH:i:s\Z', $s, new DateTimeZone( 'UTC' ) );
		return $d && $d->format( 'Y-m-d\TH:i:s\Z' ) === $s ? $d->getTimestamp() : null;
	}

	/**
	 * Charts to delete so that a new package of `incoming` bytes fits (WPP-010 …
	 * WPP-012). The oldest go first; "to keep" charts never go. With incoming = 0
	 * (daily run) only what is beyond a limit goes.
	 *
	 * @param array<int, array{id: int, start: int, bytes: int, keep: bool}> $items
	 * @param array{quota_bytes: int, max_age_days: int, max_count: int} $limits
	 * @return array{delete: int[], fits: bool}
	 */
	public static function plan_purge( array $items, int $incoming, array $limits, int $now ): array {
		$delete = array();
		$left   = array();
		$oldest = $now - $limits['max_age_days'] * 86400;
		foreach ( $items as $it ) {
			if ( ! $it['keep'] && $it['start'] < $oldest ) {
				$delete[] = $it['id'];
			} else {
				$left[] = $it;
			}
		}
		usort(
			$left,
			function ( $a, $b ) {
				return $a['start'] <=> $b['start'] ?: $a['id'] <=> $b['id'];
			}
		);
		$used  = array_sum( array_column( $left, 'bytes' ) );
		$count = count( $left );
		$new   = $incoming > 0 ? 1 : 0;
		foreach ( $left as $it ) {
			if ( $used + $incoming <= $limits['quota_bytes'] && $count + $new <= $limits['max_count'] ) {
				break;
			}
			if ( $it['keep'] ) {
				continue;
			}
			$delete[] = $it['id'];
			$used    -= $it['bytes'];
			--$count;
		}
		return array(
			'delete' => $delete,
			'fits'   => $used + $incoming <= $limits['quota_bytes'] && $count + $new <= $limits['max_count'],
		);
	}

	/** True if one more chart of `bytes` may be marked "to keep" (WPP-011). */
	public static function keep_allowed( array $items, int $bytes, int $quota ): bool {
		$kept = 0;
		foreach ( $items as $it ) {
			if ( $it['keep'] ) {
				$kept += $it['bytes'];
			}
		}
		return $kept + $bytes <= self::KEEP_SHARE * $quota;
	}

	/**
	 * Days until the quota is reached at the pace of the last 7 days (WPP-013), null
	 * when nothing came in.
	 *
	 * @param array<int, array{0: int, 1: int}> $log (time, bytes) of the ingestions.
	 */
	public static function days_to_quota( array $log, int $used, int $quota, int $now ): ?float {
		$week = 0;
		foreach ( $log as $entry ) {
			if ( $entry[0] >= $now - 7 * 86400 ) {
				$week += $entry[1];
			}
		}
		if ( $week <= 0 ) {
			return null;
		}
		return max( 0.0, ( $quota - $used ) / ( $week / 7.0 ) );
	}
}
