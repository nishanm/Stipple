// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stipple/apps/GlucoseCloud.h"
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

/// Which service, and how to log in to it. Only the fields the chosen kind
/// uses are read; the others may hold another service's settings, so switching
/// back does not mean typing them again.
struct SourceSettings {
    SourceKind kind = SourceKind::Nightscout;
    /// Nightscout: base URL and the SHA-1 of its API secret.
    std::string url;
    std::string apiSecretSha1;
    /// Dexcom username / LibreLinkUp email / Medtrum email, and its password.
    std::string username;
    std::string password;
    /// Dexcom server ("us", "ous", "jp") or LibreLinkUp region ("US", "EU"...).
    std::string region;
    /// LibreLinkUp: which followed person, when there is more than one.
    std::string patientId;

    bool operator==(const SourceSettings& other) const noexcept {
        return kind == other.kind && url == other.url && apiSecretSha1 == other.apiSecretSha1 &&
               username == other.username && password == other.password &&
               region == other.region && patientId == other.patientId;
    }
    bool operator!=(const SourceSettings& other) const noexcept { return !(*this == other); }
};

/// Polls the configured glucose service for the glucose app: Nightscout, Dexcom
/// Share, LibreLinkUp or Medtrum (see GlucoseCloud.h for each one's protocol).
///
/// One request at a time on the device's shared HTTP client, once a period,
/// never before the wall clock is set (the device boots at 1970 and no age
/// computed from that would be honest). A service that needs a login gets one
/// when it has no session, and the session is kept between periods; one poll
/// may therefore be several requests in a row (log in, then read), and a
/// session the service has dropped is renewed once within the same poll.
///
/// Nightscout and Dexcom answer with the last three hours, which replace the
/// sample set. LibreLinkUp and Medtrum answer with the latest reading, which is
/// merged in; their history is fetched only when there is a gap to fill.
///
/// The reading is rebuilt from the samples after every fetch and on every
/// minute, so a source that stops answering ages the reading into stale rather
/// than freezing it.
///
/// A refused credential - 401/403, Dexcom's AccountPasswordInvalid,
/// LibreLinkUp's status 2, Medtrum's failed login, any 429 - is held off for
/// five minutes, doubling to thirty, so a wrong password cannot lock somebody's
/// account by asking once a minute for ever. Everything else - no route, a
/// timeout, a 500, a body that is not JSON - is tried again next period.
/// Failures keep the samples; they only stop them getting newer.
class GlucoseSource {
public:
    /// Three hours of five-minute readings, plus two for the ones Dexcom
    /// Share repeats. The same count the reference renderer asks for.
    static constexpr int kEntryCount = 38;

    /// Real entries carry a dozen or more fields and run to several hundred
    /// bytes each; 38 of them is well over the client's kilobyte default and
    /// comfortably under this. The device measures the real size
    /// (`Status::lastBodyBytes`) so the number can be revisited with evidence.
    static constexpr std::size_t kMaxBodyBytes = 32u * 1024u;

    /// LibreLinkUp's graph is twelve hours of readings with nine fields each.
    /// Kept under the JSON parser's 64 KB input ceiling; a graph larger than
    /// this is dropped for the latest reading alone rather than failing.
    static constexpr std::size_t kMaxGraphBodyBytes = 60u * 1024u;

    /// Longest base URL accepted, so the composed request stays inside the
    /// client's URL limit with the path and query appended.
    static constexpr std::size_t kMaxBaseUrlBytes = 200;

    static constexpr int kMinPollSeconds = 30;
    static constexpr int kMaxPollSeconds = 600;

    /// The cloud services are asked at most once a minute whatever the
    /// setting: they are shared with a phone app and a lockout is per account.
    static constexpr int kMinCloudPollSeconds = 60;

    /// How long a fetch may run before it is abandoned. The client has its own
    /// socket timeout; this is the backstop for an adapter that lost track.
    static constexpr std::uint64_t kFetchTimeoutMillis = 20000;

    static constexpr std::uint64_t kFatalHoldMinMillis = 5u * 60u * 1000u;
    static constexpr std::uint64_t kFatalHoldMaxMillis = 30u * 60u * 1000u;

    /// Parse tokens: one per container, key and value. 38 Nightscout entries
    /// need about 1,250; a LibreLinkUp graph about 3,000.
    static constexpr int kTokenCapacity = 4096;

    /// Requests in one poll: log in, renew a dropped session once, read,
    /// fill a gap. Anything past this is a loop and stops.
    static constexpr int kMaxStepsPerPoll = 6;

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
        /// "timeout", "bad data", "no data", "truncated", "wrong password",
        /// "choose a patient", or the client's words.
        char lastFailure[48] = {};
        /// LibreLinkUp: who this account follows, for the settings page's
        /// picker, and the region actually in use after any redirect.
        LibrePatient patients[kMaxLibrePatients];
        int patientCount = 0;
        char region[8] = {};
    };

    /// The request for a base URL: `/api/v1/entries.json?count=38&find[type]=sgv`.
    static std::string entriesUrl(std::string_view baseUrl);

    /// Idempotent: the host calls this every tick with the current settings,
    /// and only a change reschedules anything (and drops any session).
    void configure(const SourceSettings& settings, int pollSeconds);

    /// Nightscout, the way it always was configured. An empty URL disables it.
    void configure(std::string_view baseUrl, std::string_view apiSecretSha1, int pollSeconds);

    /// Whether the chosen service has what it needs to be asked: a URL for
    /// Nightscout, a username and password for the others.
    bool configured() const noexcept;

    SourceKind kind() const noexcept { return settings_.kind; }

    void setClient(platform::IHttpClient* client) noexcept { client_ = client; }
    void setNetworkUp(bool up) noexcept { networkUp_ = up; }

    /// Drive it. `nowUnix` is the wall clock, valid only when `wallClockValid`.
    void tick(std::uint64_t nowMillis, std::int64_t nowUnix, bool wallClockValid,
              int utcOffsetSeconds);

    const Reading& reading() const noexcept { return reading_; }

    /// Bumped whenever `reading()` changes, so a redraw check costs a compare.
    std::uint32_t revision() const noexcept { return revision_; }

    const Status& status() const noexcept { return status_; }

    /// The Nightscout URL being fetched, or the last cloud URL asked, for
    /// tests and the log. Never a header, a body or a credential.
    std::string_view requestUrl() const noexcept {
        return settings_.kind == SourceKind::Nightscout ? std::string_view(url_)
                                                        : std::string_view(lastUrl_);
    }

private:
    enum class Step : std::uint8_t {
        None,
        NightscoutEntries,
        DexcomAccount,
        DexcomSession,
        DexcomReadings,
        LibreLogin,
        LibreConnections,
        LibreGraph,
        MedtrumLogin,
        MedtrumMonitor,
        MedtrumHistory,
    };

    /// What one answer means for the poll.
    struct Outcome {
        enum class Kind : std::uint8_t { Next, Samples, Fail } kind = Kind::Fail;
        Step next = Step::None;
        const char* reason = "bad data";
        bool fatal = false;
        /// For Samples: replace the set with `incoming_`, or keep the merged
        /// set as it stands.
        bool replace = false;
        int count = 0;
    };

    Step firstStep() const noexcept;
    void start(std::uint64_t nowMillis);
    void collect(std::uint64_t nowMillis);
    Outcome answer(int httpStatus, std::string_view body, std::string_view cookies);
    Outcome answerNightscout(int httpStatus, std::string_view body);
    Outcome answerDexcom(int httpStatus, std::string_view body);
    Outcome answerLibre(int httpStatus, std::string_view body);
    Outcome answerMedtrum(int httpStatus, std::string_view body, std::string_view cookies);
    void succeed(std::uint64_t nowMillis, const Outcome& outcome);
    void fail(const char* reason, std::uint64_t nowMillis, bool fatal);
    void dropSession() noexcept;
    void rebuild(std::int64_t nowUnix, bool wallClockValid, int utcOffsetSeconds);
    int effectivePollSeconds() const noexcept;
    std::int64_t newestEpoch() const noexcept;

    SourceSettings settings_;
    std::string url_;
    int pollSeconds_ = 60;

    platform::IHttpClient* client_ = nullptr;
    bool networkUp_ = false;
    bool running_ = false;
    std::uint64_t startedMillis_ = 0;
    std::uint64_t dueMillis_ = 0;
    std::int64_t nowUnix_ = 0;

    /// The step in flight, how many this poll has taken, and whether this
    /// poll already renewed a session (once is a renewal, twice is a loop).
    Step step_ = Step::None;
    int stepsThisPoll_ = 0;
    bool renewedThisPoll_ = false;

    /// What a request needs to stay alive through begin(): the URL, body and
    /// header values it points at.
    std::string lastUrl_;
    std::string requestBody_;
    std::string headerValues_[3];

    /// Sessions, kept between polls, dropped on a settings change.
    std::string dexcomAccountId_;
    std::string dexcomSessionId_;
    std::string libreToken_;
    std::int64_t libreExpires_ = 0;
    std::string libreAccountHash_;
    std::string librePatientId_;
    std::string libreRegion_;
    std::string medtrumCookie_;
    std::string medtrumUser_;

    Sample samples_[kMaxHistory] = {};
    int sampleCount_ = 0;
    Sample incoming_[kMaxHistory] = {};
    /// The latest reading of a service that answers with one, held while the
    /// gap behind it is fetched.
    Sample pendingCurrent_;

    Reading reading_;
    std::uint32_t revision_ = 0;
    bool dirty_ = true;
    std::int64_t lastMinute_ = -1;
    bool lastClockValid_ = false;

    Status status_;
    json::Token tokens_[kTokenCapacity] = {};
};

/// The name it had when Nightscout was the only service.
using NightscoutSource = GlucoseSource;

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
