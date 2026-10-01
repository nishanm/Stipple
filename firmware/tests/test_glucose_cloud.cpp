// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cloud glucose services: Dexcom Share, LibreLinkUp, Medtrum EasyView.
// Parsers against recorded answer shapes, then each login-and-read flow end to
// end against the simulator's HTTP client.
#include <string>

#include "stipple/apps/GlucoseCloud.h"
#include "stipple/apps/GlucoseSource.h"
#include "stipple/core/Sha256.h"
#include "stipple/platform/simulator/SimulatorHttpClient.h"
#include "support/TestFramework.h"

using stipple::Sha256;
using stipple::apps::glucose::GlucoseSource;
using stipple::apps::glucose::LibreConnections;
using stipple::apps::glucose::LibreLogin;
using stipple::apps::glucose::Sample;
using stipple::apps::glucose::SourceKind;
using stipple::apps::glucose::SourceSettings;
using stipple::apps::glucose::Trend;
using stipple::json::Token;
using stipple::platform::simulator::SimulatorHttpClient;
namespace cloud = stipple::apps::glucose;

namespace {

/// 2026-09-27 12:00:00 UTC.
constexpr std::int64_t kNow = 1790510400;

Token tokens[4096];

struct Rig {
    SimulatorHttpClient client;
    GlucoseSource source;
    std::uint64_t now = 1000;

    Rig() {
        source.setClient(&client);
        source.setNetworkUp(true);
    }

    void serve(const std::string& url, const std::string& body, int status = 200,
               const std::string& cookie = {}) {
        SimulatorHttpClient::Route route;
        route.url = url;
        route.status = status;
        route.body = body;
        route.latencyMillis = 50;
        route.setCookie = cookie;
        client.answer(route);
    }

    std::int64_t unix() const { return kNow + static_cast<std::int64_t>(now / 1000u); }

    void run(std::uint64_t millis) {
        const std::uint64_t until = now + millis;
        while (now < until) {
            source.tick(now, unix(), true, 0);
            now += 33;
        }
        source.tick(now, unix(), true, 0);
    }

    int asked(const std::string& url) const {
        int n = 0;
        for (const std::string& each : client.asked()) {
            n += each == url ? 1 : 0;
        }
        return n;
    }
};

bool has(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- Dexcom fixtures -------------------------------------------------------------

const char* const kDexAccountUrl =
    "https://share1.dexcom.com/ShareWebServices/Services/General/AuthenticatePublisherAccount";
const char* const kDexSessionUrl =
    "https://share1.dexcom.com/ShareWebServices/Services/General/LoginPublisherAccountById";
const char* const kDexReadingsUrl =
    "https://share1.dexcom.com/ShareWebServices/Services/Publisher/"
    "ReadPublisherLatestGlucoseValues?sessionId=5a5a5a5a-1111-2222-3333-444444444444"
    "&minutes=180&maxCount=38";

std::string dexReadings() {
    // Newest first, as Dexcom sends them, with the eleven-second twin.
    return R"j([{"WT":"Date(1790510340000)","ST":"Date(1790510340000)","DT":"Date(1790510340000-0400)","Value":131,"Trend":"FortyFiveUp"},)j"
           R"j({"WT":"Date(1790510329000)","ST":"Date(1790510329000)","DT":"Date(1790510329000-0400)","Value":131,"Trend":"FortyFiveUp"},)j"
           R"j({"WT":"Date(1790510040000)","ST":"Date(1790510040000)","DT":"Date(1790510040000-0400)","Value":124,"Trend":"Flat"}])j";
}

SourceSettings dexcom() {
    SourceSettings s;
    s.kind = SourceKind::Dexcom;
    s.username = "parent@example.com";
    s.password = "not-a-real-password";
    s.region = "us";
    return s;
}

// --- LibreLinkUp fixtures --------------------------------------------------------

const char* const kLluLogin = "https://api-us.libreview.io/llu/auth/login";
const char* const kLluEuLogin = "https://api-eu.libreview.io/llu/auth/login";
const char* const kLluConnections = "https://api-us.libreview.io/llu/connections";
const char* const kLluGraph = "https://api-us.libreview.io/llu/connections/p-1/graph";

std::string lluLogin() {
    return R"({"status":0,"data":{"user":{"id":"user-42","country":"US"},)"
           R"("authTicket":{"token":"tok-abc","expires":1790596800,"duration":86400000}}})";
}

std::string lluConnections(const char* time, int value, int arrow, bool twoPatients = false) {
    std::string body = R"({"status":0,"data":[{"patientId":"p-1","firstName":"Sam","lastName":"M",)"
                       R"("glucoseMeasurement":{"FactoryTimestamp":")";
    body += time;
    body += R"(","ValueInMgPerDl":)";
    body += std::to_string(value);
    body += R"(,"TrendArrow":)";
    body += std::to_string(arrow);
    body += "}}";
    if (twoPatients) {
        body += R"(,{"patientId":"p-2","firstName":"Alex","lastName":"M",)"
                R"("glucoseMeasurement":{"FactoryTimestamp":"9/27/2026 12:00:00 PM","ValueInMgPerDl":99,"TrendArrow":3}})";
    }
    body += "]}";
    return body;
}

std::string lluGraph() {
    return R"({"status":0,"data":{"connection":{"patientId":"p-1","glucoseMeasurement":)"
           R"({"FactoryTimestamp":"9/27/2026 12:00:00 PM","ValueInMgPerDl":140,"TrendArrow":4}},)"
           R"("graphData":[{"FactoryTimestamp":"9/27/2026 11:45:00 AM","ValueInMgPerDl":120},)"
           R"({"FactoryTimestamp":"9/27/2026 11:50:00 AM","ValueInMgPerDl":128},)"
           R"({"FactoryTimestamp":"9/27/2026 11:55:00 AM","ValueInMgPerDl":134}]}})";
}

SourceSettings libre() {
    SourceSettings s;
    s.kind = SourceKind::LibreLinkUp;
    s.username = "parent@example.com";
    s.password = "not-a-real-password";
    s.region = "US";
    return s;
}

// --- Medtrum fixtures ------------------------------------------------------------

std::string medtrumMonitor(double glucose, std::int64_t at) {
    return R"({"res":"OK","monitorlist":[{"username":"sam_m","sensor_status":{"glucose":)" +
           std::to_string(glucose) + R"(,"glucoseRate":1,"updateTime":)" + std::to_string(at) +
           "}}]}";
}

SourceSettings medtrum() {
    SourceSettings s;
    s.kind = SourceKind::Medtrum;
    s.username = "parent@example.com";
    s.password = "p&ss word";
    return s;
}

}  // namespace

// --- helpers ----------------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, SourceNamesAreTheTc001s) {
    SourceKind kind = SourceKind::Nightscout;
    STIPPLE_CHECK(cloud::sourceKindFromName("librelinkup", kind));
    STIPPLE_CHECK(kind == SourceKind::LibreLinkUp);
    STIPPLE_CHECK_FALSE(cloud::sourceKindFromName("carelink", kind));
    STIPPLE_CHECK_EQ(std::string(cloud::sourceKindName(SourceKind::Medtrum)), std::string("medtrum"));
}

STIPPLE_TEST(GlucoseCloud, EncodersEscapeWhatTheyMust) {
    STIPPLE_CHECK_EQ(cloud::jsonQuoted("a\"b\\c\n"), std::string("\"a\\\"b\\\\c\\n\""));
    STIPPLE_CHECK_EQ(cloud::formEncoded("p&ss word+@"), std::string("p%26ss%20word%2B%40"));
}

STIPPLE_TEST(GlucoseCloud, MergeTreatsANearTwinAsTheSameReading) {
    Sample s[4];
    int n = 0;
    n = cloud::mergeSample(s, n, 4, Sample{1000, 100, Trend::None});
    n = cloud::mergeSample(s, n, 4, Sample{1010, 101, Trend::Flat});  // same reading, newer copy
    STIPPLE_CHECK_EQ(n, 1);
    STIPPLE_CHECK_EQ(s[0].sgv, 101);
    STIPPLE_CHECK(s[0].trend == Trend::Flat);
    n = cloud::mergeSample(s, n, 4, Sample{700, 90, Trend::None});
    n = cloud::mergeSample(s, n, 4, Sample{1300, 110, Trend::None});
    STIPPLE_CHECK_EQ(n, 3);
    STIPPLE_CHECK_EQ(s[0].epoch, std::int64_t{700});
    STIPPLE_CHECK_EQ(s[2].epoch, std::int64_t{1300});
    n = cloud::mergeSample(s, n, 4, Sample{1600, 0, Trend::None});  // not a reading
    STIPPLE_CHECK_EQ(n, 3);
}

// --- Dexcom -----------------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, DexcomParsesDatesIdsAndErrors) {
    STIPPLE_CHECK_EQ(cloud::dexcomDateSeconds("Date(1790510340000)"), std::int64_t{1790510340});
    STIPPLE_CHECK_EQ(cloud::dexcomDateSeconds("Date(1790510340000-0400)"), std::int64_t{1790510340});
    STIPPLE_CHECK_EQ(cloud::dexcomDateSeconds("1790510340000"), std::int64_t{0});
    STIPPLE_CHECK_EQ(cloud::dexcomQuotedId("\"5a5a5a5a-1111-2222-3333-444444444444\""),
                     std::string("5a5a5a5a-1111-2222-3333-444444444444"));
    STIPPLE_CHECK(cloud::dexcomQuotedId("\"00000000-0000-0000-0000-000000000000\"").empty());
    STIPPLE_CHECK(cloud::dexcomQuotedId("{\"Code\":\"x\"}").empty());
    STIPPLE_CHECK(cloud::dexcomCredentialsRejected(R"({"Code":"AccountPasswordInvalid"})"));
    STIPPLE_CHECK(cloud::dexcomSessionExpired(R"({"Code":"SessionIdNotFound"})"));
    const std::string body = cloud::dexcomAccountBody("a\"b", "pw", "jp");
    STIPPLE_CHECK(has(body, R"("accountName":"a\"b")"));
    STIPPLE_CHECK(has(body, "d8665ade-9673-4e27-9ff6-92db4ce13d13"));
}

STIPPLE_TEST(GlucoseCloud, DexcomReadingsAreSortedAndKeepTheTwin) {
    Sample out[8];
    const int n = cloud::parseDexcomReadings(dexReadings(), out, 8, tokens, 4096);
    STIPPLE_REQUIRE(n == 3);
    STIPPLE_CHECK_EQ(out[0].sgv, 124);
    STIPPLE_CHECK_EQ(out[2].sgv, 131);
    STIPPLE_CHECK(out[2].trend == Trend::FortyFiveUp);
    STIPPLE_CHECK_EQ(cloud::parseDexcomReadings("{}", out, 8, tokens, 4096), -1);
}

STIPPLE_TEST(GlucoseCloud, DexcomLogsInTwiceThenReadsAndKeepsTheSession) {
    Rig rig;
    rig.serve(kDexAccountUrl, "\"9e9e9e9e-aaaa-bbbb-cccc-dddddddddddd\"");
    rig.serve(kDexSessionUrl, "\"5a5a5a5a-1111-2222-3333-444444444444\"");
    rig.serve(kDexReadingsUrl, dexReadings());
    rig.source.configure(dexcom(), 60);
    STIPPLE_CHECK(rig.source.configured());
    rig.run(2000);

    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 131);
    STIPPLE_CHECK(rig.source.reading().trend == Trend::FortyFiveUp);
    STIPPLE_REQUIRE(rig.client.askedMethods().size() == 3);
    STIPPLE_CHECK_EQ(rig.client.askedMethods()[0], std::string("POST"));
    STIPPLE_CHECK(has(rig.client.askedBodies()[0], R"("accountName":"parent@example.com")"));
    STIPPLE_CHECK(has(rig.client.askedBodies()[1], R"("accountId":"9e9e9e9e-aaaa-bbbb-cccc-dddddddddddd")"));
    STIPPLE_CHECK(has(rig.client.askedAllHeaders()[0], "Content-Type: application/json"));

    // Next period: only the readings, the session is kept.
    rig.run(61000);
    STIPPLE_CHECK_EQ(rig.asked(kDexAccountUrl), 1);
    STIPPLE_CHECK_EQ(rig.asked(kDexReadingsUrl), 2);
    // Never in the URL the source reports.
    STIPPLE_CHECK_FALSE(has(std::string(rig.source.requestUrl()), "not-a-real-password"));
}

STIPPLE_TEST(GlucoseCloud, DexcomRenewsAnExpiredSessionOnceInTheSamePoll) {
    Rig rig;
    rig.serve(kDexAccountUrl, "\"9e9e9e9e-aaaa-bbbb-cccc-dddddddddddd\"");
    rig.serve(kDexSessionUrl, "\"5a5a5a5a-1111-2222-3333-444444444444\"");
    rig.serve(kDexReadingsUrl, dexReadings());
    rig.source.configure(dexcom(), 60);
    rig.run(2000);
    STIPPLE_REQUIRE(rig.source.reading().sgv == 131);

    rig.serve(kDexReadingsUrl, R"({"Code":"SessionIdNotFound","Message":"Session not active"})", 500);
    rig.run(61000);
    // Readings refused, session renewed, readings asked again - and refused
    // again, which is a plain failure, not a loop.
    STIPPLE_CHECK_EQ(rig.asked(kDexSessionUrl), 2);
    STIPPLE_CHECK_EQ(rig.asked(kDexReadingsUrl), 3);
    STIPPLE_CHECK_EQ(rig.source.status().fatalStreak, 0);
    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 131);  // the samples stay
}

STIPPLE_TEST(GlucoseCloud, DexcomWrongPasswordIsHeldOff) {
    Rig rig;
    rig.serve(kDexAccountUrl, R"({"Code":"AccountPasswordInvalid","Message":"..."})", 500);
    rig.source.configure(dexcom(), 60);
    rig.run(2000);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().lastFailure), std::string("wrong password"));
    STIPPLE_CHECK_EQ(rig.source.status().fatalStreak, 1);
    rig.run(4 * 60 * 1000);
    STIPPLE_CHECK_EQ(rig.asked(kDexAccountUrl), 1);  // five minutes, not one
}

// --- LibreLinkUp --------------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, LibreTimestampsAreUtcWithAmPm) {
    STIPPLE_CHECK_EQ(cloud::libreTimestampSeconds("11/14/2023 10:13:20 PM"), std::int64_t{1700000000});
    STIPPLE_CHECK_EQ(cloud::libreTimestampSeconds("9/27/2026 12:00:00 PM"), kNow);
    STIPPLE_CHECK_EQ(cloud::libreTimestampSeconds("9/27/2026 12:00:00 AM"), kNow - 12 * 3600);
    STIPPLE_CHECK_EQ(cloud::libreTimestampSeconds("not a date"), std::int64_t{0});
    STIPPLE_CHECK(cloud::libreTrend(5) == Trend::SingleUp);
    STIPPLE_CHECK(cloud::libreTrend(3) == Trend::Flat);
    STIPPLE_CHECK(cloud::libreHost("eu2") != nullptr);
    STIPPLE_CHECK(cloud::libreHost("XX") == nullptr);
}

STIPPLE_TEST(GlucoseCloud, LibreLoginAndConnectionsParse) {
    LibreLogin login;
    STIPPLE_REQUIRE(cloud::parseLibreLogin(lluLogin(), login, tokens, 4096));
    STIPPLE_CHECK_EQ(login.status, 0);
    STIPPLE_CHECK_EQ(login.token, std::string("tok-abc"));
    STIPPLE_CHECK_EQ(login.userId, std::string("user-42"));
    STIPPLE_CHECK_EQ(login.expires, std::int64_t{1790596800});

    STIPPLE_REQUIRE(cloud::parseLibreLogin(R"({"status":0,"data":{"redirect":true,"region":"eu"}})",
                                           login, tokens, 4096));
    STIPPLE_CHECK(login.redirect);
    STIPPLE_CHECK_EQ(login.region, std::string("EU"));

    LibreConnections connections;
    STIPPLE_REQUIRE(cloud::parseLibreConnections(lluConnections("9/27/2026 12:00:00 PM", 140, 4),
                                                 "", connections, tokens, 4096));
    STIPPLE_CHECK_EQ(connections.patientCount, 1);
    STIPPLE_CHECK_EQ(connections.chosen, 0);
    STIPPLE_CHECK_EQ(connections.current.sgv, 140);
    STIPPLE_CHECK(connections.current.trend == Trend::FortyFiveUp);
    STIPPLE_CHECK_EQ(std::string(connections.patients[0].name), std::string("Sam M"));

    // Two people and nobody chosen: no guess.
    STIPPLE_REQUIRE(cloud::parseLibreConnections(
        lluConnections("9/27/2026 12:00:00 PM", 140, 4, true), "", connections, tokens, 4096));
    STIPPLE_CHECK_EQ(connections.patientCount, 2);
    STIPPLE_CHECK_EQ(connections.chosen, -1);
    STIPPLE_REQUIRE(cloud::parseLibreConnections(
        lluConnections("9/27/2026 12:00:00 PM", 140, 4, true), "p-2", connections, tokens, 4096));
    STIPPLE_CHECK_EQ(connections.chosen, 1);
    STIPPLE_CHECK_EQ(connections.current.sgv, 99);
}

STIPPLE_TEST(GlucoseCloud, LibreLogsInFillsTheGapThenMergesEachMinute) {
    Rig rig;
    rig.serve(kLluLogin, lluLogin());
    rig.serve(kLluConnections, lluConnections("9/27/2026 12:00:00 PM", 140, 4));
    rig.serve(kLluGraph, lluGraph());
    rig.source.configure(libre(), 60);
    rig.run(2000);

    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 140);
    STIPPLE_CHECK(rig.source.reading().trend == Trend::FortyFiveUp);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 4);
    STIPPLE_REQUIRE(rig.client.askedAllHeaders().size() == 3);
    const std::string& headers = rig.client.askedAllHeaders()[1];
    STIPPLE_CHECK(has(headers, "product: llu.ios"));
    STIPPLE_CHECK(has(headers, "Authorization: Bearer tok-abc"));
    const std::string hash = Sha256::hex("user-42", 7);
    STIPPLE_CHECK(has(headers, ("account-id: " + hash).c_str()));

    // A minute on: the latest reading only, merged; no login, no graph.
    rig.serve(kLluConnections, lluConnections("9/27/2026 12:01:00 PM", 143, 3));
    rig.run(61000);
    STIPPLE_CHECK_EQ(rig.asked(kLluLogin), 1);
    STIPPLE_CHECK_EQ(rig.asked(kLluGraph), 1);
    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 143);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 5);
    // And the same reading again a minute later is not a sixth sample.
    rig.run(61000);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 5);
}

STIPPLE_TEST(GlucoseCloud, LibreFollowsARegionRedirect) {
    Rig rig;
    rig.serve(kLluLogin, R"({"status":0,"data":{"redirect":true,"region":"eu"}})");
    rig.serve(kLluEuLogin, lluLogin());
    rig.serve("https://api-eu.libreview.io/llu/connections",
              lluConnections("9/27/2026 12:00:00 PM", 140, 4));
    rig.serve("https://api-eu.libreview.io/llu/connections/p-1/graph", lluGraph());
    rig.source.configure(libre(), 60);
    rig.run(3000);
    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 140);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().region), std::string("EU"));
}

STIPPLE_TEST(GlucoseCloud, LibreWithSeveralPeopleAsksToChoose) {
    Rig rig;
    rig.serve(kLluLogin, lluLogin());
    rig.serve(kLluConnections, lluConnections("9/27/2026 12:00:00 PM", 140, 4, true));
    rig.source.configure(libre(), 60);
    rig.run(2000);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().lastFailure), std::string("choose a patient"));
    STIPPLE_CHECK_EQ(rig.source.status().patientCount, 2);
    STIPPLE_CHECK_EQ(rig.source.reading().historyCount, 0);
}

STIPPLE_TEST(GlucoseCloud, LibreWrongPasswordIsHeldOff) {
    Rig rig;
    rig.serve(kLluLogin, R"({"status":2,"error":{"message":"notAuthenticated"}})");
    rig.source.configure(libre(), 60);
    rig.run(2000);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().lastFailure), std::string("wrong password"));
    STIPPLE_CHECK_EQ(rig.source.status().fatalStreak, 1);
}

STIPPLE_TEST(GlucoseCloud, LibreLogsInAgainWhenTheTokenIsRefused) {
    Rig rig;
    rig.serve(kLluLogin, lluLogin());
    rig.serve(kLluConnections, lluConnections("9/27/2026 12:00:00 PM", 140, 4));
    rig.serve(kLluGraph, lluGraph());
    rig.source.configure(libre(), 60);
    rig.run(2000);
    rig.serve(kLluConnections, R"({"message":"unauthorized"})", 401);
    rig.run(61000);
    STIPPLE_CHECK_EQ(rig.asked(kLluLogin), 2);
}

// --- Medtrum ---------------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, MedtrumUnitsTrendsAndHistoryUrl) {
    STIPPLE_CHECK_EQ(cloud::medtrumMgdl(6.5), 117);
    STIPPLE_CHECK_EQ(cloud::medtrumMgdl(117.0), 117);
    STIPPLE_CHECK(cloud::medtrumTrend(3) == Trend::DoubleUp);
    STIPPLE_CHECK(cloud::medtrumTrend(8) == Trend::Flat);
    STIPPLE_CHECK_EQ(cloud::medtrumHistoryUrl(kNow - 3600, kNow, "sam_m"),
                     std::string("https://easyview.medtrum.eu/mobile/ajax/download?flag=sg"
                                 "&st=2026-09-27%2011:00:00&et=2026-09-27%2012:00:00&user_name=sam_m"));
    STIPPLE_CHECK(has(cloud::medtrumLoginBody("a@b.c", "p&ss word"), "password=p%26ss%20word"));
}

STIPPLE_TEST(GlucoseCloud, MedtrumLogsInWithACookieReadsAndFillsTheGap) {
    Rig rig;
    rig.serve(std::string(cloud::kMedtrumLoginUrl), R"({"res":"OK"})", 200, "JSESSIONID=abc123");
    rig.serve(std::string(cloud::kMedtrumMonitorUrl), medtrumMonitor(7.0, kNow));
    // The history request carries the window it asks for; match its prefix.
    rig.client.answerMatching("flag=sg", 200,
                              R"({"res":"OK","data":[[0,1790509500,0,6.4],[0,1790509800,0,6.6]]})");
    rig.source.configure(medtrum(), 60);
    rig.run(2000);

    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 126);
    STIPPLE_CHECK(rig.source.reading().trend == Trend::FortyFiveUp);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 3);
    // Login, monitor, then three hours of history an hour at a time.
    STIPPLE_REQUIRE(rig.client.askedAllHeaders().size() == 5);
    STIPPLE_CHECK(has(rig.client.askedAllHeaders()[1], "Cookie: JSESSIONID=abc123"));
    STIPPLE_CHECK(has(rig.client.askedAllHeaders()[0], "User-Agent: okhttp/3.5.0"));
    STIPPLE_CHECK(has(rig.client.asked()[2], "user_name=sam_m"));
    STIPPLE_CHECK(has(rig.client.asked()[2], "&st=2026-09-27%2009:00:01&et=2026-09-27%2010:00:01"));
    STIPPLE_CHECK(has(rig.client.asked()[4], "&st=2026-09-27%2011:00:03"));
}

STIPPLE_TEST(GlucoseCloud, MedtrumRefusedLoginIsHeldOff) {
    Rig rig;
    rig.serve(std::string(cloud::kMedtrumLoginUrl), R"({"res":"ERR","resdetail":"bad"})");
    rig.source.configure(medtrum(), 60);
    rig.run(2000);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().lastFailure), std::string("wrong password"));
    STIPPLE_CHECK_EQ(rig.source.status().fatalStreak, 1);
}

// --- all of them --------------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, ACloudSourceIsAskedNoMoreThanOnceAMinute) {
    Rig rig;
    rig.serve(kDexAccountUrl, "\"9e9e9e9e-aaaa-bbbb-cccc-dddddddddddd\"");
    rig.serve(kDexSessionUrl, "\"5a5a5a5a-1111-2222-3333-444444444444\"");
    rig.serve(kDexReadingsUrl, dexReadings());
    rig.source.configure(dexcom(), 30);
    rig.run(2000);
    rig.run(45000);
    STIPPLE_CHECK_EQ(rig.asked(kDexReadingsUrl), 1);
    rig.run(20000);
    STIPPLE_CHECK_EQ(rig.asked(kDexReadingsUrl), 2);
}

STIPPLE_TEST(GlucoseCloud, SwitchingServiceDropsTheOldReadings) {
    Rig rig;
    rig.serve(kDexAccountUrl, "\"9e9e9e9e-aaaa-bbbb-cccc-dddddddddddd\"");
    rig.serve(kDexSessionUrl, "\"5a5a5a5a-1111-2222-3333-444444444444\"");
    rig.serve(kDexReadingsUrl, dexReadings());
    rig.source.configure(dexcom(), 60);
    rig.run(2000);
    STIPPLE_REQUIRE(rig.source.reading().sgv == 131);
    rig.source.configure(libre(), 60);  // nothing served for it
    rig.run(2000);
    STIPPLE_CHECK_EQ(rig.source.reading().historyCount, 0);
}

STIPPLE_TEST(GlucoseCloud, AnIncompleteLoginIsNotConfigured) {
    GlucoseSource source;
    SourceSettings s = dexcom();
    s.password.clear();
    source.configure(s, 60);
    STIPPLE_CHECK_FALSE(source.configured());
    s = libre();
    s.region = "ZZ";
    source.configure(s, 60);
    STIPPLE_CHECK_FALSE(source.configured());
}

// --- what review found -------------------------------------------------------------

STIPPLE_TEST(GlucoseCloud, AChangeOfSourceDropsTheAnswerStillInFlight) {
    // Site A asked, the owner switches to site B before A answers: A's
    // reading must never be shown as B's.
    Rig rig;
    SimulatorHttpClient::Route slow;
    slow.url = "http://a.example/api/v1/entries.json?count=38&find[type]=sgv";
    slow.body = R"([{"sgv":250,"date":1790510340000,"direction":"Flat"}])";
    slow.latencyMillis = 5000;
    rig.client.answer(slow);
    rig.source.configure("http://a.example", "", 60);
    rig.run(1000);  // A's request is in flight
    rig.source.configure("http://b.example", "", 60);
    rig.run(8000);
    STIPPLE_CHECK(rig.source.reading().sgv != 250);
}

STIPPLE_TEST(GlucoseCloud, ANamedPatientWhoIsNoLongerFollowedIsNotReplaced) {
    LibreConnections connections;
    STIPPLE_REQUIRE(cloud::parseLibreConnections(lluConnections("9/27/2026 12:00:00 PM", 140, 4),
                                                 "p-gone", connections, tokens, 4096));
    STIPPLE_CHECK_EQ(connections.chosen, -1);
}

STIPPLE_TEST(GlucoseCloud, AnEmptyMedtrumAnswerIsNoDataNotAReLogin) {
    Rig rig;
    rig.serve(std::string(cloud::kMedtrumLoginUrl), R"({"res":"OK"})", 200, "JSESSIONID=abc123");
    rig.serve(std::string(cloud::kMedtrumMonitorUrl), R"({"res":"OK","monitorlist":[]})");
    rig.source.configure(medtrum(), 60);
    rig.run(2000);
    rig.run(61000);
    STIPPLE_CHECK_EQ(rig.asked(std::string(cloud::kMedtrumLoginUrl)), 1);
    STIPPLE_CHECK_EQ(std::string(rig.source.status().lastFailure), std::string("no data"));
}

STIPPLE_TEST(GlucoseCloud, MinuteReadingsAreThinnedSoTheHistorySpansHours) {
    Sample s[64];
    int n = 0;
    for (int minute = 0; minute < 180; ++minute) {
        n = cloud::mergeSample(s, n, 64, Sample{kNow + minute * 60, 100 + minute % 7, Trend::Flat},
                               cloud::kCloudSampleSpacingSeconds);
    }
    STIPPLE_CHECK(n < 64);
    STIPPLE_CHECK(s[n - 1].epoch - s[0].epoch >= 170 * 60);  // three hours, not one
    STIPPLE_CHECK_EQ(s[n - 1].epoch, kNow + 179 * 60);       // and always the latest
}
