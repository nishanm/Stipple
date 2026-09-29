// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stipple/apps/GlucoseModel.h"
#include "stipple/json/Json.h"

namespace stipple {

namespace platform {
class IHttpClient;
}

namespace apps {
namespace glucose {

/// Nightscout's `direction` field, by name ("Flat", "FortyFiveUp",
/// "NOT COMPUTABLE" ...). Case and spaces are ignored; anything unknown is None.
Trend trendFromDirection(std::string_view direction) noexcept;

/// The same field as a number, which some uploaders send.
Trend trendFromDirectionIndex(std::int64_t index) noexcept;

/// Read a Nightscout `entries.json` body into samples, oldest first.
///
/// An entry counts when it has an `sgv` in 1..1000 and a `date` (or `mills`)
/// in milliseconds; anything else - calibrations, malformed rows - is skipped.
/// Returns how many samples were written, or -1 when the body is not a JSON
/// array at all. When more entries arrive than `capacity`, the oldest are the
/// ones dropped. `tokens` is the caller's parse buffer; nothing here allocates
/// beyond the short-lived string a direction name needs.
int parseEntries(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                 int tokenCapacity) noexcept;

/// Polls a Nightscout server for the glucose app.
///
/// One request at a time on the device's shared HTTP client, once a period,
/// never before the wall clock is set (the device boots at 1970 and no age
/// computed from that would be honest). Every successful fetch replaces the
/// whole sample set - each answer is the last three hours - and the reading is
/// rebuilt from the samples after every fetch and on every minute, so a source
/// that stops answering ages the reading into stale rather than freezing it.
///
/// A 401 or 403 is a credential problem and is held off for five minutes,
/// doubling to thirty, so a bad secret cannot hammer somebody's server once a
/// minute for ever. Everything else - no route, a timeout, a 500, a body that
/// is not JSON - is tried again next period. Failures keep the samples; they
/// only stop them getting newer.
class NightscoutSource {
public:
    /// Three hours of five-minute readings, plus two for the ones Dexcom
    /// Share repeats. The same count the reference renderer asks for.
    static constexpr int kEntryCount = 38;

    /// Real entries carry a dozen or more fields and run to several hundred
    /// bytes each; 38 of them is well over the client's kilobyte default and
    /// comfortably under this. The device measures the real size
    /// (`Status::lastBodyBytes`) so the number can be revisited with evidence.
    static constexpr std::size_t kMaxBodyBytes = 32u * 1024u;

    /// Longest base URL accepted, so the composed request stays inside the
    /// client's URL limit with the path and query appended.
    static constexpr std::size_t kMaxBaseUrlBytes = 200;

    static constexpr int kMinPollSeconds = 30;
    static constexpr int kMaxPollSeconds = 600;

    /// How long a fetch may run before it is abandoned. The client has its own
    /// socket timeout; this is the backstop for an adapter that lost track.
    static constexpr std::uint64_t kFetchTimeoutMillis = 20000;

    static constexpr std::uint64_t kFatalHoldMinMillis = 5u * 60u * 1000u;
    static constexpr std::uint64_t kFatalHoldMaxMillis = 30u * 60u * 1000u;

    /// Parse tokens: one per container, key and value. Real entries have
    /// 12-16 keys, so 38 of them need about 1,250; the headroom is for an
    /// uploader that adds a few more.
    static constexpr int kTokenCapacity = 2048;

    struct Status {
        std::uint64_t lastAttemptMillis = 0;
        std::uint64_t lastSuccessMillis = 0;
        std::uint64_t holdUntilMillis = 0;
        std::uint32_t fetches = 0;
        std::uint32_t failures = 0;
        int lastHttpStatus = 0;
        std::size_t lastBodyBytes = 0;
        int sampleCount = 0;
        int fatalStreak = 0;
        /// Short, for the panel and the diagnostics page: "http 401",
        /// "timeout", "bad data", "no data", "truncated", or the client's words.
        char lastFailure[48] = {};
    };

    /// The request for a base URL: `/api/v1/entries.json?count=38&find[type]=sgv`.
    static std::string entriesUrl(std::string_view baseUrl);

    /// Idempotent: the host calls this every tick with the current settings,
    /// and only a change reschedules anything. An empty URL disables the source.
    void configure(std::string_view baseUrl, std::string_view apiSecretSha1, int pollSeconds);

    bool configured() const noexcept { return !url_.empty(); }

    void setClient(platform::IHttpClient* client) noexcept { client_ = client; }
    void setNetworkUp(bool up) noexcept { networkUp_ = up; }

    /// Drive it. `nowUnix` is the wall clock, valid only when `wallClockValid`.
    void tick(std::uint64_t nowMillis, std::int64_t nowUnix, bool wallClockValid,
              int utcOffsetSeconds);

    const Reading& reading() const noexcept { return reading_; }

    /// Bumped whenever `reading()` changes, so a redraw check costs a compare.
    std::uint32_t revision() const noexcept { return revision_; }

    const Status& status() const noexcept { return status_; }

    /// The URL being fetched, for tests and the log. Never the header.
    std::string_view requestUrl() const noexcept { return url_; }

private:
    void start(std::uint64_t nowMillis);
    void collect(std::uint64_t nowMillis);
    void succeed(std::uint64_t nowMillis, int count);
    void fail(const char* reason, std::uint64_t nowMillis, bool fatal);
    void rebuild(std::int64_t nowUnix, bool wallClockValid, int utcOffsetSeconds);

    std::string url_;
    std::string headerValue_;
    int pollSeconds_ = 60;

    platform::IHttpClient* client_ = nullptr;
    bool networkUp_ = false;
    bool running_ = false;
    std::uint64_t startedMillis_ = 0;
    std::uint64_t dueMillis_ = 0;

    Sample samples_[kMaxHistory] = {};
    int sampleCount_ = 0;
    Sample incoming_[kMaxHistory] = {};

    Reading reading_;
    std::uint32_t revision_ = 0;
    bool dirty_ = true;
    std::int64_t lastMinute_ = -1;
    bool lastClockValid_ = false;

    Status status_;
    json::Token tokens_[kTokenCapacity] = {};
};

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
