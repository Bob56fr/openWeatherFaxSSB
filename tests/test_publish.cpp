// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Publication queue of the web showcase (VITRINE-WEB.md § 4.5, lot 5). The site and
// the clock are simulated: the lot's acceptance test (a 2 h Internet cut loses no chart
// and makes no duplicate) runs here in a few milliseconds.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

#include "owfax/vitrine/publish.hpp"
#include "owfax_test.hpp"

using namespace owfax::vitrine;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kT0 = 1794305244;  // 2026-11-10T10:07:24Z

fs::path fresh_dir(const std::string& name) {
    const fs::path d = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

void put(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int count_of(const std::string& text, const std::string& what) {
    int n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1)) ++n;
    return n;
}

/// A package as owfax-vitrine package writes it (the contents do not matter here).
fs::path make_package(const fs::path& out, const std::string& id, bool publish = true,
                      bool with_audio = true) {
    const fs::path d = out / id;
    fs::create_directories(d);
    put(d / "meta.json", "{\"chart_id\": \"" + id + "\", \"selection\": {\"publish\": " +
                             (publish ? "true" : "false") + ", \"reasons\": []}}");
    put(d / "telemetry.json", "{}");
    put(d / "raw.webp", "RIFF....WEBP");
    put(d / "thumb.webp", "RIFF....WEBP");
    if (with_audio) put(d / "audio.m4a", "....ftypM4A ");
    return d;
}

/// The WordPress plugin as the station sees it: idempotent on chart_id (PUB-061).
struct FakeSite {
    bool up = true;
    bool lose_answer = false;           ///< Stores the chart, the answer never arrives.
    std::map<std::string, int> stored;  ///< chart_id -> times created
    int calls = 0;

    SendResult operator()(const QueueItem& item) {
        ++calls;
        if (!up) return classify_answer(0, "", "Could not resolve host: example.org");
        const bool known = stored.count(item.chart_id) != 0;
        if (!known) stored[item.chart_id] = 1;
        if (lose_answer) {
            lose_answer = false;
            return classify_answer(0, "", "Operation timed out after 600000 milliseconds");
        }
        return classify_answer(known ? 200 : 201,
                               std::string("{\"chart_id\":\"") + item.chart_id +
                                   "\",\"result\":\"" + (known ? "exists" : "created") +
                                   "\",\"url\":\"https://example.org/fax/x/\"}",
                               "");
    }
};

}  // namespace

OWFAX_TEST(publish_retry_delay_from_1_min_to_6_h) {
    OWFAX_CHECK(retry_delay_s(0) == 0);
    OWFAX_CHECK(retry_delay_s(1) == 60);
    OWFAX_CHECK(retry_delay_s(2) == 120);
    OWFAX_CHECK(retry_delay_s(9) == 60 * 256);
    OWFAX_CHECK(retry_delay_s(10) == 6 * 3600);
    OWFAX_CHECK(retry_delay_s(1000) == 6 * 3600);
}

OWFAX_TEST(publish_classifies_the_site_answers) {
    const auto created =
        classify_answer(201, R"({"result":"created","url":"https://s/fax/a/"})", "");
    OWFAX_CHECK(created.status == SendStatus::kSent && created.url == "https://s/fax/a/");
    OWFAX_CHECK(classify_answer(200, R"({"result":"exists"})", "").status == SendStatus::kSent);
    // A page that is not the plugin (wrong address, captive portal) is not a success.
    OWFAX_CHECK(classify_answer(200, "<html>Welcome</html>", "").status == SendStatus::kRetry);
    const auto bad = classify_answer(
        422, R"({"code":"owfax_bad_meta","message":"chart_id: bad","data":{"status":422}})", "");
    OWFAX_CHECK(bad.status == SendStatus::kRefused);
    OWFAX_CHECK(bad.detail.find("owfax_bad_meta") != std::string::npos);
    for (int http : {400, 413, 415}) {
        OWFAX_CHECK(classify_answer(http, "{}", "").status == SendStatus::kRefused);
    }
    for (int http : {401, 403, 404, 408, 429, 500, 502, 503, 507}) {
        OWFAX_CHECK(classify_answer(http, "{}", "").status == SendStatus::kRetry);
    }
    OWFAX_CHECK(classify_answer(0, "", "Connection refused").status == SendStatus::kRetry);
}

OWFAX_TEST(publish_enqueues_only_complete_publishable_packages) {
    const fs::path out = fresh_dir("owfax_pub_enqueue_out");
    PublishQueue q(fresh_dir("owfax_pub_enqueue_q"));
    std::string err;
    const auto good = make_package(out, "20261110T1007Z_DDK6_13882.5kHz");
    OWFAX_CHECK(q.enqueue(good, kT0, err) == "queued" && err.empty());
    OWFAX_CHECK(q.enqueue(good, kT0, err) == "already queued");
    OWFAX_CHECK(q.pending().size() == 1);
    OWFAX_CHECK(fs::exists(q.root() / "pending" / "20261110T1007Z_DDK6_13882.5kHz" / "audio.m4a"));

    OWFAX_CHECK(
        q.enqueue(make_package(out, "20261110T1107Z_DDK6_13882.5kHz", false), kT0, err).empty());
    OWFAX_CHECK(err.find("not publishable") != std::string::npos);
    OWFAX_CHECK(
        q.enqueue(make_package(out, "20261110T1207Z_DDK6_13882.5kHz", true, false), kT0, err)
            .empty());
    OWFAX_CHECK(err.find("audio.m4a") != std::string::npos);
    fs::create_directories(out / "notes");
    OWFAX_CHECK(q.enqueue(out / "notes", kT0, err).empty());
    OWFAX_CHECK(q.pending().size() == 1);
}

// Acceptance test of lot 5: a 2 h Internet cut loses no chart, and no chart is
// stored twice once the connection is back.
OWFAX_TEST(publish_two_hour_cut_loses_nothing_and_duplicates_nothing) {
    const fs::path out = fresh_dir("owfax_pub_cut_out");
    PublishQueue q(fresh_dir("owfax_pub_cut_q"));
    FakeSite site;
    std::int64_t now = kT0;
    const auto clock = [&] { return now; };
    const char* ids[] = {"20261110T1007Z_DDK6_13882.5kHz", "20261110T1047Z_DDK3_7880kHz",
                         "20261110T1127Z_GYA_8040kHz"};

    site.up = false;  // the cut starts with the first chart
    std::string err;
    int tries_during_cut = 0;
    for (std::int64_t minute = 0; minute <= 4 * 60; ++minute) {
        now = kT0 + minute * 60;
        if (minute == 0 || minute == 40 || minute == 80) {
            OWFAX_CHECK(q.enqueue(make_package(out, ids[minute / 40]), now, err) == "queued");
        }
        if (minute == 120) site.up = true;  // back after 2 h
        const int before = site.calls;
        q.run(clock, std::ref(site));  // the sender wakes up every minute
        if (minute < 120) tries_during_cut += site.calls - before;
    }
    OWFAX_CHECK(q.pending().empty());
    OWFAX_CHECK(site.stored.size() == 3);
    for (const char* id : ids) {
        OWFAX_CHECK(site.stored[id] == 1);
        OWFAX_CHECK(q.was_sent(id));
    }
    // 1, 2, 4 … 64 min: a handful of tries during the cut, not one a minute.
    OWFAX_CHECK(tries_during_cut >= 5 && tries_during_cut <= 8);
    OWFAX_CHECK(q.failures() == 0);
    const std::string journal = slurp(q.root() / "journal.log");
    OWFAX_CHECK(count_of(journal, ": sent (created") == 3);
    OWFAX_CHECK(count_of(journal, "dropped") == 0);
    OWFAX_CHECK(journal.find("Could not resolve host") != std::string::npos);
}

OWFAX_TEST(publish_lost_answer_is_sent_again_without_duplicate) {
    const fs::path out = fresh_dir("owfax_pub_lost_out");
    PublishQueue q(fresh_dir("owfax_pub_lost_q"));
    FakeSite site;
    site.lose_answer = true;
    std::int64_t now = kT0;
    const auto clock = [&] { return now; };
    std::string err;
    q.enqueue(make_package(out, "20261110T1007Z_DDK6_13882.5kHz"), now, err);
    auto r = q.run(clock, std::ref(site));
    OWFAX_CHECK(r.stopped && r.sent == 0 && r.next_try_unix == kT0 + 60);
    now += 30;
    r = q.run(clock, std::ref(site));  // too early
    OWFAX_CHECK(r.waited && site.calls == 1);
    now += 30;
    r = q.run(clock, std::ref(site));
    OWFAX_CHECK(r.sent == 1);
    OWFAX_CHECK(site.stored["20261110T1007Z_DDK6_13882.5kHz"] == 1);
    OWFAX_CHECK(count_of(slurp(q.root() / "sent.log"), "20261110T1007Z_DDK6_13882.5kHz") == 1);
    // Queued again later (same package, another run of the tool): not sent twice.
    OWFAX_CHECK(q.enqueue(out / "20261110T1007Z_DDK6_13882.5kHz", now, err) == "already sent");
}

OWFAX_TEST(publish_refused_package_is_set_aside_and_the_next_goes) {
    const fs::path out = fresh_dir("owfax_pub_refused_out");
    PublishQueue q(fresh_dir("owfax_pub_refused_q"));
    std::string err;
    q.enqueue(make_package(out, "20261110T1007Z_DDK6_13882.5kHz"), kT0, err);
    q.enqueue(make_package(out, "20261110T1107Z_DDK6_13882.5kHz"), kT0, err);
    int calls = 0;
    const auto r =
        q.run([] { return kT0; },
              [&](const QueueItem& item) {
                  ++calls;
                  if (item.chart_id.rfind("20261110T1007Z", 0) == 0) {
                      return classify_answer(422, R"({"code":"owfax_bad_meta","message":"x"})", "");
                  }
                  return classify_answer(201, R"({"result":"created"})", "");
              });
    OWFAX_CHECK(calls == 2 && r.refused == 1 && r.sent == 1);
    OWFAX_CHECK(q.pending().empty());
    const auto refused = q.refused();
    OWFAX_CHECK(refused.size() == 1 &&
                refused[0].last_error.find("owfax_bad_meta") != std::string::npos);
    // Queued again by hand (after a fix): it leaves refused/.
    OWFAX_CHECK(q.enqueue(out / "20261110T1007Z_DDK6_13882.5kHz", kT0, err) == "queued");
    OWFAX_CHECK(q.refused().empty());
}

OWFAX_TEST(publish_drops_after_48_hours_with_a_journal_line) {
    const fs::path out = fresh_dir("owfax_pub_drop_out");
    PublishQueue q(fresh_dir("owfax_pub_drop_q"));
    FakeSite site;
    site.up = false;
    std::int64_t now = kT0;
    const auto clock = [&] { return now; };
    std::string err;
    q.enqueue(make_package(out, "20261110T1007Z_DDK6_13882.5kHz"), now, err);
    int dropped = 0;
    for (; now <= kT0 + 49 * 3600; now += 60) dropped += q.run(clock, std::ref(site)).dropped;
    OWFAX_CHECK(dropped == 1 && q.pending().empty());
    // Never more than 6 h between two tries: about 13 tries in 48 h.
    OWFAX_CHECK(site.calls >= 12 && site.calls <= 17);
    const std::string journal = slurp(q.root() / "journal.log");
    OWFAX_CHECK(journal.find("dropped, not sent after 48 h") != std::string::npos);
    // The package itself stays where owfax-vitrine wrote it (local gallery, PUB-026).
    OWFAX_CHECK(fs::exists(out / "20261110T1007Z_DDK6_13882.5kHz" / "audio.m4a"));
}

OWFAX_TEST(publish_queue_survives_a_restart) {
    const fs::path out = fresh_dir("owfax_pub_restart_out");
    const fs::path root = fresh_dir("owfax_pub_restart_q");
    std::string err;
    {
        PublishQueue q(root);
        q.enqueue(make_package(out, "20261110T1007Z_DDK6_13882.5kHz"), kT0, err);
        FakeSite down;
        down.up = false;
        q.run([] { return kT0; }, std::ref(down));
        q.run([] { return kT0 + 60; }, std::ref(down));
    }
    PublishQueue q(root);  // the station restarted
    OWFAX_CHECK(q.pending().size() == 1 && q.pending()[0].attempts == 2);
    OWFAX_CHECK(q.failures() == 2 && q.next_try_unix() == kT0 + 60 + 120);
    FakeSite site;
    OWFAX_CHECK(q.run([] { return kT0 + 120; }, std::ref(site)).waited);
    OWFAX_CHECK(q.run([] { return kT0 + 180; }, std::ref(site)).sent == 1);

    // Stopped after the site took a chart, before it left the queue: not sent again.
    const auto again = make_package(out, "20261110T1107Z_DDK6_13882.5kHz");
    q.enqueue(again, kT0, err);
    {
        std::ofstream log(root / "sent.log", std::ios::app);
        log << "2026-11-10T11:30:00Z\t20261110T1107Z_DDK6_13882.5kHz\tcreated\t\n";
    }
    const int before = site.calls;
    q.run([] { return kT0 + 240; }, std::ref(site));
    OWFAX_CHECK(site.calls == before && q.pending().empty());

    q.write_status("https://example.org", kT0 + 240, true);
    const std::string status = slurp(root / "status.json");
    OWFAX_CHECK(status.find("\"active\": true") != std::string::npos);
    OWFAX_CHECK(status.find("\"pending\": 0") != std::string::npos);
    OWFAX_CHECK(status.find("20261110T1107Z_DDK6_13882.5kHz") != std::string::npos);
}

OWFAX_TEST(publish_reads_credentials) {
    const fs::path d = fresh_dir("owfax_pub_cred");
    Credentials c;
    std::string err;
    put(d / "ok", "owfax-station:abcd EFGH ijkl MNOP qrst UVWX\n");
    fs::permissions(d / "ok", fs::perms::owner_read | fs::perms::owner_write);
    OWFAX_CHECK(read_credentials(d / "ok", c, err));
    OWFAX_CHECK(c.login == "owfax-station" && c.password == "abcd EFGH ijkl MNOP qrst UVWX");
    put(d / "bad", "no colon here\n");
    fs::permissions(d / "bad", fs::perms::owner_read | fs::perms::owner_write);
    OWFAX_CHECK(!read_credentials(d / "bad", c, err));
    OWFAX_CHECK(!read_credentials(d / "missing", c, err));
#ifndef _WIN32
    fs::permissions(d / "ok", fs::perms::others_read, fs::perm_options::add);
    OWFAX_CHECK(!read_credentials(d / "ok", c, err));
    OWFAX_CHECK(err.find("chmod 600") != std::string::npos);
#endif
}
