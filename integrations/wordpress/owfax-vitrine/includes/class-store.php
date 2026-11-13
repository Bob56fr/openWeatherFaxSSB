<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Package files in wp-content/uploads/owfax/<chart_id>/, outside the media library
// (WPP-002), served as static files: the player never calls PHP (VIT-002).

defined( 'ABSPATH' ) || exit;

final class Owfax_Vitrine_Store {
	const DIR = 'owfax';

	public static function base_dir(): string {
		$u = wp_upload_dir( null, false );
		return trailingslashit( $u['basedir'] ) . self::DIR;
	}

	public static function base_url(): string {
		$u = wp_upload_dir( null, false );
		return trailingslashit( $u['baseurl'] ) . self::DIR;
	}

	public static function chart_dir( string $chart_id ): string {
		return self::base_dir() . '/' . $chart_id;
	}

	/** URL of a package directory, with a trailing slash (the player's `base`). */
	public static function chart_url( string $chart_id ): string {
		return self::base_url() . '/' . rawurlencode( $chart_id ) . '/';
	}

	/**
	 * Creates uploads/owfax/ with its protections (WPP-003): no script runs there,
	 * no directory listing, and long cache headers for files that never change for
	 * a given chart id (WPP-045). nginx needs the same by hand (see readme.txt).
	 */
	public static function ensure(): void {
		$dir = self::base_dir();
		if ( ! is_dir( $dir ) ) {
			wp_mkdir_p( $dir );
		}
		$htaccess = <<<'HTACCESS'
# openWeatherFaxSSB showcase: static chart packages only (written by the plugin).
Options -Indexes -ExecCGI
<FilesMatch "\.(php[0-9]?|phtml|phar|pl|py|cgi|sh|shtml)$">
  <IfModule mod_authz_core.c>
    Require all denied
  </IfModule>
  <IfModule !mod_authz_core.c>
    Order allow,deny
    Deny from all
  </IfModule>
</FilesMatch>
<IfModule mod_mime.c>
  AddType image/webp .webp
  AddType audio/mp4 .m4a
  AddType application/json .json
</IfModule>
<IfModule mod_headers.c>
  Header set X-Content-Type-Options "nosniff"
  <FilesMatch "\.(webp|m4a)$">
    Header set Cache-Control "public, max-age=31536000, immutable"
  </FilesMatch>
  <FilesMatch "\.json$">
    Header set Cache-Control "public, max-age=86400"
  </FilesMatch>
</IfModule>

HTACCESS;
		if ( ! file_exists( $dir . '/.htaccess' ) || file_get_contents( $dir . '/.htaccess' ) !== $htaccess ) {
			file_put_contents( $dir . '/.htaccess', $htaccess );
		}
		if ( ! file_exists( $dir . '/index.html' ) ) {
			file_put_contents( $dir . '/index.html', '' );
		}
	}

	/** Removes a package directory, and nothing outside uploads/owfax/. */
	public static function delete( string $chart_id ): void {
		if ( ! Owfax_Vitrine_Plugin::is_chart_id( $chart_id ) ) {
			return;
		}
		self::remove_tree( self::chart_dir( $chart_id ) );
	}

	public static function remove_tree( string $dir ): void {
		$base = realpath( self::base_dir() );
		$real = realpath( $dir );
		if ( false === $base || false === $real || 0 !== strpos( $real, $base . DIRECTORY_SEPARATOR ) ) {
			return;
		}
		foreach ( (array) scandir( $real ) as $name ) {
			if ( '.' === $name || '..' === $name ) {
				continue;
			}
			$path = $real . DIRECTORY_SEPARATOR . $name;
			if ( is_dir( $path ) && ! is_link( $path ) ) {
				self::remove_tree( $path );
			} else {
				@unlink( $path );
			}
		}
		@rmdir( $real );
	}

	public static function size( string $chart_id ): int {
		$total = 0;
		foreach ( (array) glob( self::chart_dir( $chart_id ) . '/*' ) as $f ) {
			$total += (int) @filesize( $f );
		}
		return $total;
	}
}
