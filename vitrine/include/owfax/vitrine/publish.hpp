// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Publication queue of the web showcase (docs/VITRINE-WEB.md § 4.5, lot 5): the
// packages waiting to be sent to the WordPress plugin, kept on disk so that they
// survive a restart and an Internet cut (PUB-062).
//
//   <queue>/pending/<chart_id>/   a copy of the package, plus item.json
//   <queue>/refused/<chart_id>/   refused for good by the site (bad package), with why
//   <queue>/state.json            failures in a row and the time of the next try
//   <queue>/sent.log              one line per chart the site holds: never sent twice
//   <queue>/journal.log           every event, UTC; never the password (PUB-063)
//   <queue>/status.json           what the station's dashboard shows (VIT-003)
//
// One send at a time, oldest chart first. A failure that may pass (no network, site
// down, rate limit, quota full) stops the round; the next one waits 1 min, then twice
// as long each time, up to 6 h. A package waiting for more than 48 h is dropped with a
// line in the journal. The site is idempotent on chart_id (PUB-061): a package sent
// again after a lost answer comes back "exists" and counts as sent.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace owfax::vitrine {

struct QueuePolicy {
    std::int64_t first_delay_s = 60;           ///< PUB-062: 1 min …
    std::int64_t max_delay_s = 6 * 3600;       ///< … to 6 h.
    std::int64_t give_up_after_s = 48 * 3600;  ///< Dropped after 48 h in the queue.
};

/// Delay before the next try after `failures` failures in a row (1, 2, 4 … min, 6 h max).
std::int64_t retry_delay_s(int failures, const QueuePolicy& policy = {});

enum class SendStatus {
    kSent,     ///< The site holds the chart (created now, or already there).
    kRetry,    ///< May pass: try again later.
    kRefused,  ///< The site will never take this package as it is.
};

struct SendResult {
    SendStatus status = SendStatus::kRetry;
    int http = 0;        ///< 0: no answer (network, DNS, TLS, timeout).
    std::string detail;  ///< Short reason for the journal.
    std::string url;     ///< Page of the chart on the site, when sent.
};

/// Reads the site's answer: HTTP status, body (JSON), transport error (curl's message).
SendResult classify_answer(int http, const std::string& body, const std::string& transport_error);

/// The files of a package, in the multipart fields the plugin expects.
struct PackageFile {
    const char* field;
    const char* name;
    bool required;
};
const std::vector<PackageFile>& package_files();

struct QueueItem {
    std::string chart_id;
    std::filesystem::path dir;
    std::int64_t enqueued_unix = 0;
    int attempts = 0;
    std::string last_error;
};

struct RoundReport {
    int sent = 0;
    int refused = 0;
    int dropped = 0;
    bool waited = false;   ///< Nothing tried: the backoff delay is not over.
    bool stopped = false;  ///< A failure that may pass stopped the round.
    std::int64_t next_try_unix = 0;
};

class PublishQueue {
public:
    using Sender = std::function<SendResult(const QueueItem&)>;
    using Clock = std::function<std::int64_t()>;

    explicit PublishQueue(std::filesystem::path root, QueuePolicy policy = {});

    /// Copies a package directory into the queue. Returns "queued", "already queued" or
    /// "already sent"; "" with `error` set if the package cannot be published (not
    /// publishable, not encoded, files missing).
    std::string enqueue(const std::filesystem::path& package, std::int64_t now, std::string& error);

    /// Charts waiting, oldest first.
    std::vector<QueueItem> pending() const;
    std::vector<QueueItem> refused() const;
    bool was_sent(const std::string& chart_id) const;

    /// One round: drops what waited too long, then sends while it works. `now` is read
    /// again after each send (sends take time). `force` ignores the backoff delay.
    RoundReport run(const Clock& now, const Sender& send, bool force = false);

    int failures() const;
    std::int64_t next_try_unix() const;

    /// Writes status.json for the dashboard.
    void write_status(const std::string& site, std::int64_t now, bool active) const;

    /// Appends one line to journal.log (and returns it, for the console).
    std::string log(std::int64_t now, const std::string& line) const;

    const std::filesystem::path& root() const { return root_; }

    /// Called with every journal line (the console of owfax-vitrine send).
    std::function<void(const std::string&)> on_log;

private:
    void save_state(int failures, std::int64_t next_try, const std::string& last_error) const;
    void save_item(const QueueItem& item) const;

    std::filesystem::path root_;
    QueuePolicy policy_;
};

/// "2026-11-13T09:05:00Z".
std::string utc_text(std::int64_t unix_s);

/// Account and password for the site, from a file holding "login:application password"
/// on its first line. On POSIX the file must not be readable by others (PUB-063).
struct Credentials {
    std::string login;
    std::string password;
};
bool read_credentials(const std::filesystem::path& file, Credentials& out, std::string& error);

}  // namespace owfax::vitrine
