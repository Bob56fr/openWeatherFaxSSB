=== openWeatherFaxSSB — Vitrine ===
Requires at least: 6.0
Requires PHP: 7.4
Stable tag: 0.1.0
License: AGPL-3.0-or-later
License URI: https://www.gnu.org/licenses/agpl-3.0.html

Replays the weather fax charts received by an openWeatherFaxSSB station, slowly, with
their real sound, S-meter and signal quality. Never live: a showcase of what the
software receives (docs/VITRINE-WEB.md in the openWeatherFaxSSB repository).

== Description ==

The station sends each complete chart that passes its checks (complete, good enough,
no local disturbance, weather service allowing it). The site stores it outside the
media library, in wp-content/uploads/owfax/<chart id>/, and replays it:

* the image appears line by line, at the pace of the real sound;
* S-meter (or audio level for an SDR), SNR, line quality, timeline;
* every chart has its page, /fax/<chart id>/, with its details, the weather service's
  attribution, and Open Graph tags for sharing.

During playback the browser only reads static files: no PHP call, nothing to do for
WordPress when nobody visits.

Shortcodes, also available as blocks:

* `[owfax_player]` the latest chart; `[owfax_player playlist="10" rate="10"]` the 10
  latest in a loop, starting by themselves (muted) when they come into view;
  `station="dwd-pinneberg"` for one station only.
* `[owfax_gallery limit="24"]` thumbnails with date, distance and quality, filterable by
  station.
* `[owfax_chart id="20261110T1007Z_DDK6_13882.5kHz"]` one chart.
* `[owfax_stations]` the sources received, distance, charts, best quality.

== Installation ==

1. Copy the owfax-vitrine directory to wp-content/plugins/ and activate the plugin.
2. Under Weather fax > Settings, set the quota (default 2 GB), the maximum age (60 days)
   and number (300 charts), and whether new charts are published at once or as drafts.
3. Create a user with the role "owfax publisher". It can send charts and nothing else.
4. In that user's profile, create an application password. Give the station the user
   name and that password; it sends to https://<site>/wp-json/owfax/v1/charts.
   Application passwords need HTTPS.
5. Make sure the host accepts uploads of 16 MB at least (upload_max_filesize,
   post_max_size): GET /wp-json/owfax/v1/limits shows what it accepts.

== Retention ==

The first limit reached (quota, age, number) purges the oldest charts, each with its
sound, its files and its page. The purge runs at each new chart, before it is written,
and once a day. A chart marked "to keep" (checkbox on its edit screen) is never purged;
those charts may take 20 % of the quota at most. If only charts to keep are left, a
new chart is refused with 507 and the station keeps it in its queue.

== nginx ==

nginx ignores the .htaccess the plugin writes in uploads/owfax/. Add the equivalent to
the server block:

    location ^~ /wp-content/uploads/owfax/ {
        location ~ \.(php|phtml|phar|pl|py|cgi|sh)$ { deny all; }
        autoindex off;
        types { image/webp webp; audio/mp4 m4a; application/json json; }
        location ~ \.(webp|m4a)$ { add_header Cache-Control "public, max-age=31536000, immutable"; }
        location ~ \.json$ { add_header Cache-Control "public, max-age=86400"; }
    }

A CDN can serve that directory: the files of a chart never change.

== Uninstall ==

Deleting the plugin removes its settings and the publisher role. The charts and their
files are removed too only if "delete the charts and their files" is checked in the
settings.

== Translations ==

English and French (languages/). After changing a text:
python3 integrations/wordpress/tools/i18n.py (in the openWeatherFaxSSB repository).
