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
//   GET /api/v1/live                          WebSocket: start, lock, line, complete…
//   GET /…                                    web interface (static files)

#pragma once

#include <mutex>
#include <set>
#include <string>

#include "owfax/net/http_server.hpp"

namespace owfax::net {

struct ApiConfig {
    std::string charts_dir = ".";  ///< Where the receiver writes the charts.
    std::string catalog_dir;       ///< Station JSON files (empty = no catalogue).
    std::string web_dir;           ///< Web interface files (empty = built-in page).
    std::string input;             ///< Description of the audio input.
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
    Response chart(const std::string& stem, const std::string& part) const;
    Response stations(const std::string& id) const;
    Response web(const std::string& path) const;
    Response info() const;
    Response live();

    ApiConfig config_;
    mutable std::mutex mutex_;
    std::string status_;
    std::set<WebSocket*> clients_;
};

/// True for a chart stem or station id usable as a file name: letters, digits,
/// '.', '_' and '-', not starting with '.'.
bool is_safe_name(const std::string& name);

}  // namespace owfax::net
