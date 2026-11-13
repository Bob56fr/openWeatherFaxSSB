<?php
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Uninstall (WPP-044): the charts and their files go only if the settings say so;
// the settings, logs and the publisher role always go.

defined( 'WP_UNINSTALL_PLUGIN' ) || exit;

$owfax_settings = (array) get_option( 'owfax_vitrine_settings', array() );
if ( ! empty( $owfax_settings['delete_on_uninstall'] ) ) {
	$owfax_ids = get_posts(
		array(
			'post_type'      => 'owfax_chart',
			'post_status'    => 'any',
			'posts_per_page' => -1,
			'fields'         => 'ids',
		)
	);
	foreach ( array_merge( $owfax_ids, get_posts( array( 'post_type' => 'owfax_chart', 'post_status' => 'trash', 'posts_per_page' => -1, 'fields' => 'ids' ) ) ) as $owfax_id ) {
		wp_delete_post( $owfax_id, true );
	}
	// The whole directory, including leftovers of interrupted ingestions.
	$owfax_upload = wp_upload_dir( null, false );
	$owfax_dir    = trailingslashit( $owfax_upload['basedir'] ) . 'owfax';
	if ( is_dir( $owfax_dir ) ) {
		$owfax_it = new RecursiveIteratorIterator(
			new RecursiveDirectoryIterator( $owfax_dir, FilesystemIterator::SKIP_DOTS ),
			RecursiveIteratorIterator::CHILD_FIRST
		);
		foreach ( $owfax_it as $owfax_f ) {
			$owfax_f->isDir() && ! $owfax_f->isLink() ? @rmdir( $owfax_f->getPathname() ) : @unlink( $owfax_f->getPathname() );
		}
		@rmdir( $owfax_dir );
	}
}
foreach ( array( 'owfax_vitrine_settings', 'owfax_vitrine_refusals', 'owfax_vitrine_sends', 'owfax_vitrine_ingest_log' ) as $owfax_opt ) {
	delete_option( $owfax_opt );
}
wp_clear_scheduled_hook( 'owfax_vitrine_daily' );
remove_role( 'owfax_publisher' );
$owfax_admin = get_role( 'administrator' );
if ( $owfax_admin ) {
	$owfax_admin->remove_cap( 'owfax_publish' );
}
