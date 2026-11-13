// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Local HTTP API of the reception engine, version 1 (spec ARC-002, ARC-005, DSP-007,
// UI-006, UI-008). Read-only: charts, status, station catalogue, and the live
// stream of the chart being received over WebSocket. Documented in docs/api/openapi.yaml.
//
//   GET /api/v1/info                          version, source URL (AGPL §13), input
//   GET /api/v1/status                        engine state (also written to status.json)
//   GET /api/v1/charts[?limit=N]              charts received, newest first
//   GET /api/v1/charts/{stem}                 one chart's metadata
//   GET /api/v1/charts/{stem}/{live|raw|enhanced}.png
//   GET /api/v1/stations                      station catalogue
//   GET /api/v1/stations/{id}
//   GET /api/v1/vitrine[?limit=N]             web showcase packages (their meta.json),
//                                             newest first (docs/VITRINE-WEB.md)
//   GET /api/v1/vitrine/{chart_id}/{file}     meta.json, telemetry.json, raw.webp,
//                                             enh.webp, thumb.webp, audio.m4a
//   GET /api/v1/live                          WebSocket: start, lock, line, complete,
//                                             status, spectrum (input audio)
//   GET /api/v1/diagnostics                   host, hardware tier, inputs (UI-007)
//   GET /api/v1/journal                       journal, for bug reports (UI-007)
//   GET /api/v1/settings                      station settings, version, password set?
//   PUT /api/v1/settings                      change them (skipper session, UI-002/004)
//   POST /api/v1/session, DELETE …            skipper login / logout
//   POST /api/v1/password                     create (first time) or change it
//   POST /api/v1/restart                      restart between two charts (skipper)
//   GET /…                                    web interface (static files)
// Files are served whole or by byte range (Range: bytes=…), to seek in a sound.
// Only GET and HEAD are read-only; every change needs a skipper session (NFR-006).

#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <string>

#include "owfax/net/http_server.hpp"
#include "owfax/net/settings.hpp"

namespace owfax::net {

struct ApiConfig {
    std::string charts_dir = ".";  ///< Where the receiver writes the charts.
    std::string catalog_dir;       ///< Station JSON files (empty = no catalogue).
    std::string web_dir;           ///< Web interface files (empty = built-in page).
    std::string vitrine_dir;       ///< Showcase packages, one directory each (empty = none).
    std::string input;             ///< Description of the audio input.
    std::string journal_path;      ///< Journal served at /api/v1/journal (empty = none).
    /// JSON object for /api/v1/diagnostics (host, inputs, options), built on request.
    std::function<std::string()> diagnostics;
    /// Skipper settings (UI-002). Null: settings are not editable (no --config).
    SettingsStore* settings = nullptr;
    /// Called after the settings changed (on the HTTP thread): apply them.
    std::function<void(const StationSettings&)> on_settings;
    /// Input URI in use, to tell whether a changed input needs a restart.
    std::string running_input;
    /// Asks owfaxd to stop between two charts, so that its service manager starts it
    /// again (new audio input). Null: no restart from the web interface.
    std::function<void()> on_restart;
};

class Api {
public:
    explicit Api(ApiConfig config);
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    /// Request handler to give to HttpServer. The Api must outlive the server.
    HttpServer::Handler handler();

    /// Replaces the status object returned by /api/v1/status (a JSON object).
    void set_status(std::string json_object);
    std::string status() const;

    /// Sends a JSON message to every live client (WebSocket /api/v1/live).
    void publish(const std::string& json_message);
    std::size_t live_clients() const;

    Response handle(const Request& request);

private:
    Response charts(const Request& request) const;
    Response chart(const std::string& stem, const std::string& part,
                   const std::string& query) const;
    Response stations(const std::string& id) const;
    Response web(const std::string& path, const std::string& range) const;
    Response vitrine(const std::string& id, const std::string& file, const Request& request) const;
    Response info() const;
    Response diagnostics() const;
    Response journal() const;
    Response live();
    Response settings_get(const Request& request);
    Response settings_put(const Request& request);
    Response session(const Request& request);
    Response password(const Request& request);
    Response restart(const Request& request);
    std::string bearer(const Request& request) const;
    Response settings_reply(bool logged_in, int status = 200);

    ApiConfig config_;
    mutable std::mutex mutex_;
    std::string status_;
    std::set<WebSocket*> clients_;
    Sessions sessions_;
};

/// True for a chart stem or station id usable as a file name: letters, digits,
/// '.', '_' and '-', not starting with '.'.
bool is_safe_name(const std::string& name);

}  // namespace owfax::net
