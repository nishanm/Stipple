// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stipple/apps/GlucoseModel.h"
#include "stipple/json/Json.h"

namespace stipple {
namespace apps {
namespace glucose {

/// The cloud glucose services the TC001 nightscout-clock firmware reads -
/// Dexcom Share, LibreLinkUp and Medtrum EasyView - as pure functions: the
/// endpoints, the request bodies and the parsing of each answer. Nothing here
/// touches the network; `NightscoutSource` drives the requests, and the test
/// suite drives these with recorded answers.
///
/// Ported from ktomy/nightscout-clock's BGSource*.cpp (LGPL-2.1, which permits
/// this GPL-3.0 use) and nightscout-pixbar's sources.py, so a reading means the
/// same thing on the TC001 clocks, the host renderer and this panel.

// --- which service ------------------------------------------------------------

enum class SourceKind : std::uint8_t { Nightscout, Dexcom, LibreLinkUp, Medtrum };

/// "nightscout", "dexcom", "librelinkup", "medtrum" - the TC001's names.
const char* sourceKindName(SourceKind kind) noexcept;
bool sourceKindFromName(std::string_view name, SourceKind& out) noexcept;

// --- shared helpers -----------------------------------------------------------

/// `text` as a JSON string, quotes included.
std::string jsonQuoted(std::string_view text);

/// `application/x-www-form-urlencoded` encoding of one value.
std::string formEncoded(std::string_view text);

/// Merge `sample` into `samples` (oldest first, at most `capacity`), treating
/// one within 30 seconds of a sample already held as the same reading - a
/// service polled every minute answers with the same reading five times. The
/// newer copy wins, so a trend that arrives late is kept. Returns the count.
int mergeSample(Sample* samples, int count, int capacity, const Sample& sample) noexcept;

// --- Dexcom Share -------------------------------------------------------------

/// "us", "ous" (outside the US) or "jp". Null for anything else.
const char* dexcomBaseUrl(std::string_view server) noexcept;
const char* dexcomApplicationId(std::string_view server) noexcept;

inline constexpr std::string_view kDexcomAccountPath =
    "/ShareWebServices/Services/General/AuthenticatePublisherAccount";
inline constexpr std::string_view kDexcomSessionPath =
    "/ShareWebServices/Services/General/LoginPublisherAccountById";
inline constexpr std::string_view kDexcomReadingsPath =
    "/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues";

/// The three hours the other renderers ask for, plus two readings for the
/// twins Dexcom repeats.
inline constexpr int kDexcomMinutes = 180;
inline constexpr int kDexcomMaxCount = 38;

std::string dexcomAccountBody(std::string_view username, std::string_view password,
                              std::string_view server);
std::string dexcomSessionBody(std::string_view accountId, std::string_view password,
                              std::string_view server);

/// Dexcom answers a login with a bare JSON string: `"6d2c...-..."`. Returns
/// the id, or empty when the body is not one - including the all-zeros id
/// Dexcom sends for an account that exists with nothing shared.
std::string dexcomQuotedId(std::string_view body);

/// Whether an error body says the credentials are wrong (retrying will not
/// help), and whether it says the session expired (log in again, once).
bool dexcomCredentialsRejected(std::string_view body) noexcept;
bool dexcomSessionExpired(std::string_view body) noexcept;

/// "Date(1700000000000)" or "Date(1700000000000-0500)" to unix seconds; 0 if
/// it is not one.
std::int64_t dexcomDateSeconds(std::string_view text) noexcept;

/// The readings array, oldest first. -1 when the body is not an array.
int parseDexcomReadings(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                        int tokenCapacity) noexcept;

// --- LibreLinkUp --------------------------------------------------------------

/// "US", "EU", "EU2", "DE", "FR", "JP", "AP", "AU", "AE", "CA", "LA", "RU".
/// Null for anything else. Case is ignored.
const char* libreHost(std::string_view region) noexcept;

inline constexpr std::string_view kLibreProduct = "llu.ios";
inline constexpr std::string_view kLibreVersion = "4.16.0";
inline constexpr std::string_view kLibreUserAgent =
    "Mozilla/5.0 (iPhone; CPU OS 17_4.1 like Mac OS X) AppleWebKit/536.26 (KHTML, like Gecko) "
    "Version/17.4.1 Mobile/10A5355d Safari/8536.25";

std::string libreLoginBody(std::string_view email, std::string_view password);

struct LibreLogin {
    /// LibreLinkUp's own status: 0 is success, 2 is wrong credentials, 4 is
    /// "accept the new terms in the app first".
    int status = -1;
    /// The account lives in another region; log in again there.
    bool redirect = false;
    std::string region;
    std::string token;
    std::int64_t expires = 0;  // unix seconds
    std::string userId;
};

/// False when the body is not a LibreLinkUp answer at all.
bool parseLibreLogin(std::string_view body, LibreLogin& out, json::Token* tokens,
                     int tokenCapacity);

/// "11/14/2023 10:13:20 PM" (FactoryTimestamp, which is UTC) to unix seconds;
/// 0 if it is not one.
std::int64_t libreTimestampSeconds(std::string_view text) noexcept;

/// TrendArrow 1-5 to a trend. LibreLinkUp has no double arrows.
Trend libreTrend(std::int64_t arrow) noexcept;

/// One person this account follows.
struct LibrePatient {
    char id[40] = {};
    char name[48] = {};
};

inline constexpr int kMaxLibrePatients = 4;

struct LibreConnections {
    int status = -1;
    LibrePatient patients[kMaxLibrePatients];
    int patientCount = 0;
    /// Index of the patient chosen, or -1: none configured among several, or
    /// the configured one is not followed.
    int chosen = -1;
    Sample current;
};

/// The connections list: who is followed and each one's latest reading. The
/// patient is `wantedId` when given and present, else the only one there is.
bool parseLibreConnections(std::string_view body, std::string_view wantedId,
                           LibreConnections& out, json::Token* tokens, int tokenCapacity);

/// The graph: `data.graphData` (history, no trends) and
/// `data.connection.glucoseMeasurement` (latest, with trend), oldest first.
/// -1 when the body is not a LibreLinkUp answer.
int parseLibreGraph(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                    int tokenCapacity) noexcept;

// --- Medtrum EasyView ---------------------------------------------------------

inline constexpr std::string_view kMedtrumLoginUrl = "https://easyview.medtrum.eu/mobile/ajax/login";
inline constexpr std::string_view kMedtrumMonitorUrl =
    "https://easyview.medtrum.eu/mobile/ajax/monitor?flag=monitor_list";
inline constexpr std::string_view kMedtrumHistoryUrl =
    "https://easyview.medtrum.eu/mobile/ajax/download?flag=sg";
inline constexpr std::string_view kMedtrumUserAgent = "okhttp/3.5.0";
inline constexpr std::string_view kMedtrumDevInfo = "Android 12;Xiamoi vayu;Android 12";
inline constexpr std::string_view kMedtrumAppTag = "v=1.2.70(112);n=eyfo;p=android";

std::string medtrumLoginBody(std::string_view email, std::string_view password);

/// `&st=...&et=...&user_name=...` appended to the history URL, times in UTC.
std::string medtrumHistoryUrl(std::int64_t fromUnix, std::int64_t toUnix,
                              std::string_view username);

/// Medtrum sends mmol/L for some accounts and mg/dL for others; anything
/// under 30 is mmol/L.
int medtrumMgdl(double value) noexcept;

/// Glucose rate code to a trend.
Trend medtrumTrend(std::int64_t rate) noexcept;

/// `{"res":"OK"}` or not.
bool medtrumOk(std::string_view body, json::Token* tokens, int tokenCapacity);

/// The monitor list's first entry: its latest reading and its username, which
/// the history request needs.
bool parseMedtrumMonitor(std::string_view body, Sample& current, std::string& username,
                         json::Token* tokens, int tokenCapacity);

/// The history rows, oldest first. -1 when the body is not an OK answer.
int parseMedtrumHistory(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                        int tokenCapacity) noexcept;

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
