// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseSource.h"

#include <cstring>
#include <string>

#include "stipple/apps/GlucoseModel.h"
#include "stipple/core/Sha1.h"
#include "stipple/platform/simulator/SimulatorHttpClient.h"
#include "support/TestFramework.h"

using stipple::Sha1;
using stipple::apps::glucose::deltaFor;
using stipple::apps::glucose::NightscoutSource;
using stipple::apps::glucose::parseEntries;
using stipple::apps::glucose::Reading;
using stipple::apps::glucose::readingFromSamples;
using stipple::apps::glucose::Sample;
using stipple::apps::glucose::Trend;
using stipple::apps::glucose::trendFromDirection;
using stipple::apps::glucose::trendFromDirectionIndex;
using stipple::json::Token;
using stipple::platform::simulator::SimulatorHttpClient;

namespace {

/// The reference renderer's own fixture (nightscout-pixbar tests/test_pixbar.py),
/// byte for byte: newest first as Nightscout sends it, with a calibration row
/// in the middle that carries no sgv.
constexpr std::int64_t kFixedNow = 1790510400;
const char* const kFixture =
    R"([{"type": "sgv", "sgv": 118, "date": 1790510340000, "direction": "Flat"}, )"
    R"({"type": "cal", "date": 1790510370000}, )"
    R"({"type": "sgv", "sgv": 115, "date": 1790510040000, "direction": "FortyFiveUp"}])";

const char* const kBase = "http://nightscout.example:1337";
const char* const kEntries =
    "http://nightscout.example:1337/api/v1/entries.json?count=38&find[type]=sgv";
const char* const kSecret = "not-a-real-secret";

std::string secretHash() {
    return Sha1::hex(kSecret, std::strlen(kSecret));
}

struct Rig {
    SimulatorHttpClient client;
    NightscoutSource source;
    std::uint64_t now = 1000;
    bool clockValid = true;

    Rig() {
        source.setClient(&client);
        source.setNetworkUp(true);
    }

    void serve(const std::string& body, int status = 200, std::uint32_t latency = 100) {
        SimulatorHttpClient::Route route;
        route.url = kEntries;
        route.status = status;
        route.body = body;
        route.latencyMillis = latency;
        client.answer(route);
    }

    void configure(int pollSeconds = 60) {
        source.configure(kBase, secretHash(), pollSeconds);
    }

    std::int64_t unix() const { return kFixedNow + static_cast<std::int64_t>(now / 1000u); }

    /// Thirty-three milliseconds a tick, like the device.
    void run(std::uint64_t millis) {
        const std::uint64_t until = now + millis;
        while (now < until) {
            source.tick(now, unix(), clockValid, 0);
            now += 33;
        }
        source.tick(now, unix(), clockValid, 0);
    }
};

int parse(const char* body, Sample* out, int capacity) {
    static Token tokens[512];
    return parseEntries(body, out, capacity, tokens, 512);
}

}  // namespace

// --- the poller ---------------------------------------------------------------

STIPPLE_TEST(GlucoseSource, FetchesTheReferenceFixtureIntoAReading) {
    Rig rig;
    rig.configure();
    rig.serve(kFixture);
    rig.run(1000);

    const NightscoutSource::Status& status = rig.source.status();
    STIPPLE_CHECK_EQ(static_cast<int>(status.fetches), 1);
    STIPPLE_CHECK_EQ(static_cast<int>(status.failures), 0);
    STIPPLE_CHECK_EQ(status.lastHttpStatus, 200);
    STIPPLE_CHECK_EQ(status.sampleCount, 2);
    STIPPLE_CHECK_EQ(static_cast<int>(status.lastBodyBytes), static_cast<int>(std::strlen(kFixture)));

    // The same answer the reference gives for the same bytes: the calibration
    // dropped, the rest sorted oldest first, the newest reading on top.
    const Reading& reading = rig.source.reading();
    STIPPLE_CHECK_EQ(reading.sgv, 118);
    STIPPLE_CHECK(reading.trend == Trend::Flat);
    STIPPLE_CHECK(reading.hasDelta);
    STIPPLE_CHECK_EQ(reading.delta, 3);
    STIPPLE_CHECK_EQ(reading.historyCount, 2);
    STIPPLE_CHECK_EQ(reading.history[0].sgv, 115);
    STIPPLE_CHECK_EQ(reading.minutesAgo, 1);
    STIPPLE_CHECK(reading.timeKnown);
    STIPPLE_CHECK_EQ(reading.hour, 12);
    STIPPLE_CHECK_EQ(reading.minute, 0);
    STIPPLE_CHECK(rig.source.revision() > 0);

    // The request is the reference's request, and the secret is not in it.
    STIPPLE_REQUIRE(!rig.client.asked().empty());
    STIPPLE_CHECK(rig.client.asked().back() == kEntries);
    STIPPLE_CHECK(rig.client.asked().back().find(kSecret) == std::string::npos);
    STIPPLE_CHECK(rig.client.askedHeaders().back() == "api-secret: " + secretHash());
    STIPPLE_CHECK_EQ(static_cast<int>(secretHash().size()), 40);
}

STIPPLE_TEST(GlucoseSource, ARefusedCredentialIsHeldOffAndDoubles) {
    Rig rig;
    rig.configure();
    rig.serve("", 401);
    rig.run(1000);

    const NightscoutSource::Status& status = rig.source.status();
    STIPPLE_CHECK_EQ(static_cast<int>(status.failures), 1);
    STIPPLE_CHECK_EQ(status.fatalStreak, 1);
    STIPPLE_CHECK(std::strcmp(status.lastFailure, "http 401") == 0);
    const std::uint64_t firstHold = status.holdUntilMillis - status.lastAttemptMillis;
    STIPPLE_CHECK(firstHold >= 300000 && firstHold < 300200);

    // Not asked again inside the hold, however many periods pass.
    rig.run(290000);
    STIPPLE_CHECK_EQ(static_cast<int>(status.fetches), 1);

    rig.run(11000);
    STIPPLE_CHECK_EQ(static_cast<int>(status.fetches), 2);
    STIPPLE_CHECK_EQ(status.fatalStreak, 2);
    const std::uint64_t secondHold = status.holdUntilMillis - status.lastAttemptMillis;
    STIPPLE_CHECK(secondHold >= 600000 && secondHold < 600200);

    // Nothing was ever received, so the reading is the explicit no-data one.
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 999);
    STIPPLE_CHECK(rig.source.reading().stale());
}

STIPPLE_TEST(GlucoseSource, BadDataKeepsTheLastReadingAndAgesIt) {
    Rig rig;
    rig.configure();
    rig.serve(kFixture);
    rig.run(1000);
    STIPPLE_REQUIRE(rig.source.reading().sgv == 118);

    // The server starts answering with a login page. Not a credential
    // failure, so no hold - and not data either.
    rig.serve("<html><body>Sign in</body></html>", 200);
    rig.run(61000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.source.status().failures), 1);
    STIPPLE_CHECK(std::strcmp(rig.source.status().lastFailure, "bad data") == 0);
    STIPPLE_CHECK_EQ(rig.source.status().fatalStreak, 0);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 2);
    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 118);
    STIPPLE_CHECK_FALSE(rig.source.reading().stale());

    // Twenty minutes on with nothing new: the same samples, honestly aged.
    rig.run(20u * 60u * 1000u);
    STIPPLE_CHECK(rig.source.reading().stale());
    STIPPLE_CHECK_EQ(rig.source.reading().sgv, 118);
    STIPPLE_CHECK(rig.source.reading().minutesAgo >= 21);
}

STIPPLE_TEST(GlucoseSource, AFetchThatNeverAnswersIsAbandoned) {
    Rig rig;
    rig.configure();
    SimulatorHttpClient::Route route;
    route.url = kEntries;
    route.hang = true;
    rig.client.answer(route);

    rig.run(21000);
    STIPPLE_CHECK(std::strcmp(rig.source.status().lastFailure, "timeout") == 0);
    STIPPLE_CHECK(rig.client.stage() == SimulatorHttpClient::Stage::Idle);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.source.status().fetches), 1);
}

STIPPLE_TEST(GlucoseSource, NothingIsFetchedBeforeTheWallClockIsValid) {
    // The device boots at 1970. An age computed from that is not an age.
    Rig rig;
    rig.configure();
    rig.serve(kFixture);
    rig.clockValid = false;
    rig.run(5000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 0);
    STIPPLE_CHECK_FALSE(rig.source.reading().timeKnown);
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 999);

    rig.clockValid = true;
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
    STIPPLE_CHECK(rig.source.reading().timeKnown);
}

STIPPLE_TEST(GlucoseSource, AnEmptyListIsNoData) {
    Rig rig;
    rig.configure();
    rig.serve("[]");
    rig.run(1000);
    STIPPLE_CHECK(std::strcmp(rig.source.status().lastFailure, "no data") == 0);
    STIPPLE_CHECK_EQ(rig.source.status().sampleCount, 0);
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 999);
}

STIPPLE_TEST(GlucoseSource, ABodyCutAtTheCapIsReportedNotParsed) {
    Rig rig;
    rig.configure();
    rig.serve(std::string(NightscoutSource::kMaxBodyBytes + 100, '['));
    rig.run(1000);
    STIPPLE_CHECK(rig.client.lastTruncated());
    STIPPLE_CHECK(std::strcmp(rig.source.status().lastFailure, "truncated") == 0);
}

STIPPLE_TEST(GlucoseSource, ConfigureIsIdempotentAndAChangeAsksAgain) {
    Rig rig;
    rig.configure();
    rig.serve(kFixture);
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    // The host calls this every tick. Same settings, nothing happens.
    for (int i = 0; i < 10; ++i) {
        rig.configure();
        rig.run(100);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    // A real change fetches straight away rather than waiting out the period.
    rig.configure(120);
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 2);
}

STIPPLE_TEST(GlucoseSource, AnUnconfiguredSourceIsNoDataAndAsksNobody) {
    Rig rig;
    rig.serve(kFixture);
    rig.run(2000);
    STIPPLE_CHECK_FALSE(rig.source.configured());
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 0);
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 999);
}

STIPPLE_TEST(GlucoseSource, TheReadingIsRefreshedOnTheMinute) {
    Rig rig;
    rig.configure(600);  // long period, so the refresh is the clock's doing
    rig.serve(kFixture);
    rig.run(1000);
    const std::uint32_t revision = rig.source.revision();
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 1);

    rig.run(60000);
    STIPPLE_CHECK(rig.source.revision() > revision);
    STIPPLE_CHECK_EQ(rig.source.reading().minutesAgo, 2);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
}

// --- the parser ---------------------------------------------------------------

STIPPLE_TEST(GlucoseSource, ParseEntriesFollowsTheReferenceRules) {
    Sample out[8];
    STIPPLE_CHECK_EQ(parse(kFixture, out, 8), 2);
    STIPPLE_CHECK_EQ(out[0].sgv, 115);
    STIPPLE_CHECK_EQ(out[0].epoch, std::int64_t{1790510040});
    STIPPLE_CHECK(out[0].trend == Trend::FortyFiveUp);
    STIPPLE_CHECK_EQ(out[1].sgv, 118);
    STIPPLE_CHECK(out[1].trend == Trend::Flat);

    // A numeric direction, a `mills` timestamp, and values no meter produces.
    STIPPLE_CHECK_EQ(parse(R"([{"sgv":100,"date":1000000,"direction":4},)"
                           R"({"sgv":90,"mills":2000000},)"
                           R"({"sgv":0,"date":3000000},{"sgv":1001,"date":4000000},)"
                           R"({"sgv":80,"direction":"Flat"}])",
                           out, 8),
                     2);
    STIPPLE_CHECK(out[0].trend == Trend::Flat);
    STIPPLE_CHECK_EQ(out[0].epoch, std::int64_t{1000});
    STIPPLE_CHECK_EQ(out[1].epoch, std::int64_t{2000});
    STIPPLE_CHECK(out[1].trend == Trend::None);

    // Not an array, or not JSON: refused, not "zero readings".
    STIPPLE_CHECK_EQ(parse(R"({"status":"ok"})", out, 8), -1);
    STIPPLE_CHECK_EQ(parse("<html>", out, 8), -1);

    // More than fits: the newest are kept, still ascending.
    STIPPLE_CHECK_EQ(parse(R"([{"sgv":5,"date":5000},{"sgv":1,"date":1000},{"sgv":4,"date":4000},)"
                           R"({"sgv":2,"date":2000},{"sgv":3,"date":3000}])",
                           out, 3),
                     3);
    STIPPLE_CHECK_EQ(out[0].sgv, 3);
    STIPPLE_CHECK_EQ(out[1].sgv, 4);
    STIPPLE_CHECK_EQ(out[2].sgv, 5);
}

STIPPLE_TEST(GlucoseSource, DirectionNamesAreTheReferenceVocabulary) {
    STIPPLE_CHECK(trendFromDirection("Flat") == Trend::Flat);
    STIPPLE_CHECK(trendFromDirection("FortyFiveUp") == Trend::FortyFiveUp);
    STIPPLE_CHECK(trendFromDirection("Forty Five Up") == Trend::FortyFiveUp);
    STIPPLE_CHECK(trendFromDirection("DOUBLEDOWN") == Trend::DoubleDown);
    STIPPLE_CHECK(trendFromDirection("SingleUp") == Trend::SingleUp);
    STIPPLE_CHECK(trendFromDirection("NOT COMPUTABLE") == Trend::None);
    STIPPLE_CHECK(trendFromDirection("RATE OUT OF RANGE") == Trend::None);
    STIPPLE_CHECK(trendFromDirection("") == Trend::None);
    STIPPLE_CHECK(trendFromDirectionIndex(1) == Trend::DoubleUp);
    STIPPLE_CHECK(trendFromDirectionIndex(7) == Trend::DoubleDown);
    STIPPLE_CHECK(trendFromDirectionIndex(0) == Trend::None);
    STIPPLE_CHECK(trendFromDirectionIndex(9) == Trend::None);
}

// --- the model ------------------------------------------------------------------

STIPPLE_TEST(GlucoseSource, DeltaFollowsTheWindowRule) {
    int delta = 0;
    const Sample plain[] = {{0, 100}, {300, 103}};
    STIPPLE_CHECK(deltaFor(plain, 2, delta));
    STIPPLE_CHECK_EQ(delta, 3);

    // Dexcom's twin reading eleven seconds later must not zero the delta.
    const Sample twins[] = {{0, 100}, {11, 100}, {300, 103}};
    STIPPLE_CHECK(deltaFor(twins, 3, delta));
    STIPPLE_CHECK_EQ(delta, 3);

    // Moved both ways inside the window: no honest single number.
    const Sample bothWays[] = {{0, 100}, {150, 110}, {300, 105}};
    STIPPLE_CHECK_FALSE(deltaFor(bothWays, 3, delta));

    // Too big to be a delta rather than a sensor change.
    const Sample jump[] = {{0, 100}, {300, 250}};
    STIPPLE_CHECK_FALSE(deltaFor(jump, 2, delta));

    // Dense (Libre-style) data: just the last two.
    const Sample dense[] = {{0, 100}, {60, 101}, {120, 102}, {180, 103}, {240, 104}, {300, 105}};
    STIPPLE_CHECK(deltaFor(dense, 6, delta));
    STIPPLE_CHECK_EQ(delta, 1);

    // One sample, or a gap wider than the window: nothing to compare with.
    STIPPLE_CHECK_FALSE(deltaFor(plain, 1, delta));
    const Sample gap[] = {{0, 100}, {1000, 110}};
    STIPPLE_CHECK_FALSE(deltaFor(gap, 2, delta));
}

STIPPLE_TEST(GlucoseSource, ReadingCarriesLocalTimeAndAge) {
    const Sample samples[] = {{kFixedNow - 360, 115, Trend::FortyFiveUp},
                              {kFixedNow - 60, 118, Trend::Flat}};
    // 12:00:00 UTC on the fixed day; four hours west of it is eight in the morning.
    const Reading reading = readingFromSamples(samples, 2, kFixedNow, -4 * 3600, true);
    STIPPLE_CHECK_EQ(reading.hour, 8);
    STIPPLE_CHECK_EQ(reading.minute, 0);
    STIPPLE_CHECK_EQ(reading.minutesAgo, 1);
    STIPPLE_CHECK_EQ(reading.sgv, 118);
    STIPPLE_CHECK(reading.trend == Trend::Flat);

    // Crossing midnight westward wraps rather than going negative.
    const Reading late = readingFromSamples(samples, 2, 3600, -7200, true);
    STIPPLE_CHECK_EQ(late.hour, 23);
    STIPPLE_CHECK_EQ(late.minute, 0);

    // A reading from a source that answered in the future is zero minutes old,
    // never negative.
    const Reading early = readingFromSamples(samples, 2, kFixedNow - 600, 0, true);
    STIPPLE_CHECK_EQ(early.minutesAgo, 0);
}
