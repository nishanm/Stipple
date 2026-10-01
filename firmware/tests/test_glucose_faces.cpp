// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which faces are in use, the default, cycling and the daily schedule: the
// "Clock faces" and "Daily schedule" cards of the TC001 settings page.
#include <string>

#include "stipple/api/ApiServer.h"
#include "stipple/api/Http.h"
#include "stipple/apps/GlucoseFacePlan.h"
#include "stipple/apps/GlucoseSource.h"
#include "stipple/config/Config.h"
#include "stipple/config/GlucoseFaceSettings.h"
#include "stipple/json/Json.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::api::ApiContext;
using stipple::api::ApiOptions;
using stipple::api::ApiServer;
using stipple::api::methodFromName;
using stipple::api::Request;
using stipple::api::Response;
using stipple::apps::GlucoseFace;
using stipple::apps::glucose::NightscoutSource;
using stipple::config::Config;
using stipple::config::ConfigStore;
using stipple::config::LoadStatus;
using stipple::platform::simulator::SimulatorPlatform;
namespace plan = stipple::apps::glucose;

namespace {

struct Fixture {
    SimulatorPlatform platform;
    Config config;
    ConfigStore store{platform.storage()};
    NightscoutSource source;
    ApiServer server;

    Fixture() : server(context(), ApiOptions{}) {}

    ApiContext context() {
        ApiContext c;
        c.config = &config;
        c.configStore = &store;
        c.platform = &platform;
        c.glucose = &source;
        return c;
    }

    int patch(const std::string& glucoseBody) {
        Request request;
        request.method = methodFromName("PATCH");
        request.path = "/api/v1/settings";
        request.body = "{\"glucose\":" + glucoseBody + "}";
        return server.handle(request, 0).status;
    }

    std::string get() {
        Request request;
        request.method = methodFromName("GET");
        request.path = "/api/v1/settings";
        return server.handle(request, 0).body;
    }
};

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

constexpr plan::FaceMask bits(GlucoseFace a) { return plan::faceBit(a); }

}  // namespace

// --- the plan ------------------------------------------------------------------

STIPPLE_TEST(GlucoseFaces, StepMovesOnlyBetweenActiveFacesAndWraps) {
    const plan::FaceMask mask =
        static_cast<plan::FaceMask>(bits(GlucoseFace::Hero) | bits(GlucoseFace::Clock) |
                                    bits(GlucoseFace::BigGraph));
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::Hero, 1) == GlucoseFace::Clock);
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::Clock, 1) == GlucoseFace::BigGraph);
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::BigGraph, 1) == GlucoseFace::Hero);
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::Hero, -1) == GlucoseFace::BigGraph);
    // A face that left the set steps to its nearest neighbour that is in it.
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::HeroDelta, 1) == GlucoseFace::Clock);
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::HeroDelta, -1) == GlucoseFace::Hero);
    // One face: every step stays on it.
    STIPPLE_CHECK(plan::stepActiveFace(bits(GlucoseFace::Clock), GlucoseFace::Clock, 1) ==
                  GlucoseFace::Clock);
    // NoData is never a stop.
    STIPPLE_CHECK(plan::stepActiveFace(mask, GlucoseFace::NoData, 1) == GlucoseFace::Hero);
}

STIPPLE_TEST(GlucoseFaces, ScheduleRowLastRowRunsOvernight) {
    const plan::ScheduleRow rows[] = {
        {7 * 60, GlucoseFace::Hero, 128},
        {20 * 60, GlucoseFace::Clock, 8},
    };
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 2, 6 * 60 + 59), 1);  // still last night
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 2, 7 * 60), 0);
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 2, 19 * 60 + 59), 0);
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 2, 20 * 60), 1);
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 2, 23 * 60 + 59), 1);
    STIPPLE_CHECK_EQ(plan::scheduleRowAt(rows, 0, 600), -1);
}

STIPPLE_TEST(GlucoseFaces, OnlyTheTc001IntervalsAreAccepted) {
    for (const int ok : {0, 10, 30, 60, 120, 180, 300}) {
        STIPPLE_CHECK(plan::cycleSecondsAllowed(ok));
    }
    for (const int bad : {-1, 1, 5, 45, 301, 600}) {
        STIPPLE_CHECK_FALSE(plan::cycleSecondsAllowed(bad));
    }
}

// --- the settings --------------------------------------------------------------

STIPPLE_TEST(GlucoseFaces, DefaultsAreAllFivePlanOff) {
    const Config fresh;
    STIPPLE_CHECK_EQ(stipple::config::faceMaskOf(fresh.glucose), plan::kAllFaces);
    STIPPLE_CHECK_EQ(fresh.glucose.cycleSeconds, 0);
    STIPPLE_CHECK_FALSE(fresh.glucose.schedule.enabled);
}

STIPPLE_TEST(GlucoseFaces, RoundTripThroughTheStore) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());
    Config written;
    written.glucose.faces = {"hero", "clock"};
    written.glucose.face = "clock";
    written.glucose.cycleSeconds = 30;
    written.glucose.schedule.enabled = true;
    written.glucose.schedule.rows = {{7 * 60, "hero", 128}, {21 * 60 + 30, "clock", -1}};
    STIPPLE_CHECK(store.save(written));

    Config read;
    STIPPLE_REQUIRE(store.load(read).status == LoadStatus::Loaded);
    STIPPLE_CHECK_EQ(read.glucose.faces.size(), std::size_t{2});
    STIPPLE_CHECK_EQ(read.glucose.face, std::string("clock"));
    STIPPLE_CHECK_EQ(read.glucose.cycleSeconds, 30);
    STIPPLE_CHECK(read.glucose.schedule.enabled);
    STIPPLE_REQUIRE(read.glucose.schedule.rows.size() == 2);
    STIPPLE_CHECK_EQ(read.glucose.schedule.rows[1].minutes, 21 * 60 + 30);
    STIPPLE_CHECK_EQ(read.glucose.schedule.rows[1].face, std::string("clock"));
    STIPPLE_CHECK_EQ(read.glucose.schedule.rows[0].brightness, 128);
    STIPPLE_CHECK_EQ(read.glucose.schedule.rows[1].brightness, -1);
}

STIPPLE_TEST(GlucoseFaces, ABrokenStoredBlockIsRepairedNotRefused) {
    // A stored file is never refused for this block: a clock with no face, or
    // a load failure that costs the Nightscout settings, is worse than any
    // repair.
    const char* stored = R"({"faces":["nope","no-data"],"face":"clock","cycleSeconds":7,
        "schedule":{"enabled":true,"rows":[{"from":"25:00","face":"hero"}]}})";
    stipple::json::Token tokens[128];
    stipple::json::Document document(tokens, 128);
    STIPPLE_REQUIRE(document.parse(stored) == stipple::json::Error::None);
    Config c;
    c.glucose.face = "clock";
    stipple::config::loadGlucoseFaceSettings(document.root(), c.glucose);
    STIPPLE_CHECK_EQ(stipple::config::faceMaskOf(c.glucose), plan::kAllFaces);
    STIPPLE_CHECK_EQ(c.glucose.face, std::string("clock"));
    STIPPLE_CHECK_EQ(c.glucose.cycleSeconds, 0);
    STIPPLE_CHECK(c.glucose.schedule.rows.empty());
    STIPPLE_CHECK_FALSE(c.glucose.schedule.enabled);
}

STIPPLE_TEST(GlucoseFaces, TheApiReportsTheBlock) {
    Fixture f;
    const std::string body = f.get();
    STIPPLE_CHECK(contains(body, "\"faces\":[\"hero\",\"hero-delta\",\"hero-graph\",\"clock\",\"big-graph\"]"));
    STIPPLE_CHECK(contains(body, "\"cycleSeconds\":0"));
    STIPPLE_CHECK(contains(body, "\"schedule\":{\"enabled\":false,\"rows\":[]}"));
}

STIPPLE_TEST(GlucoseFaces, PatchNarrowsTheSetAndMovesTheDefaultIfItHasToGo) {
    Fixture f;
    f.config.glucose.face = "hero-graph";
    STIPPLE_CHECK_EQ(f.patch(R"({"faces":["clock","hero"]})"), 200);
    // Stored in canonical order whatever order it arrived in.
    STIPPLE_REQUIRE(f.config.glucose.faces.size() == 2);
    STIPPLE_CHECK_EQ(f.config.glucose.faces[0], std::string("hero"));
    STIPPLE_CHECK_EQ(f.config.glucose.face, std::string("hero"));
}

STIPPLE_TEST(GlucoseFaces, PatchRefusesWhatCannotBeHonoured) {
    Fixture f;
    STIPPLE_CHECK_EQ(f.patch(R"({"faces":[]})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"faces":["no-data"]})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"faces":["hero"],"face":"clock"})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"faces":["hero"],"cycleSeconds":30})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"cycleSeconds":45})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"schedule":{"enabled":true,"rows":[]}})"), 422);
    STIPPLE_CHECK_EQ(f.patch(R"({"schedule":{"rows":[{"from":"7:00","face":"hero"}]}})"), 422);
    STIPPLE_CHECK_EQ(
        f.patch(R"({"schedule":{"rows":[{"from":"07:00","face":"hero","brightness":300}]}})"),
        422);
    STIPPLE_CHECK_EQ(f.patch(R"({"schedule":{"rows":[{"from":"07:00","face":"hero"},
                                                   {"from":"07:00","face":"clock"}]}})"),
                     422);
    // Nothing above changed anything.
    STIPPLE_CHECK_EQ(stipple::config::faceMaskOf(f.config.glucose), plan::kAllFaces);
    STIPPLE_CHECK_EQ(f.config.glucose.cycleSeconds, 0);
    STIPPLE_CHECK(f.config.glucose.schedule.rows.empty());
}

STIPPLE_TEST(GlucoseFaces, PatchAcceptsAScheduleAndSortsIt) {
    Fixture f;
    STIPPLE_CHECK_EQ(f.patch(R"({"schedule":{"enabled":true,"rows":[
        {"from":"21:00","face":"clock","brightness":8},
        {"from":"07:00","face":"hero","brightness":null}]}})"),
                     200);
    STIPPLE_REQUIRE(f.config.glucose.schedule.rows.size() == 2);
    STIPPLE_CHECK_EQ(f.config.glucose.schedule.rows[0].minutes, 7 * 60);
    STIPPLE_CHECK_EQ(f.config.glucose.schedule.rows[0].brightness, -1);
    STIPPLE_CHECK_EQ(f.config.glucose.schedule.rows[1].brightness, 8);
    STIPPLE_CHECK(contains(f.get(), R"({"from":"21:00","face":"clock","brightness":8})"));
}
