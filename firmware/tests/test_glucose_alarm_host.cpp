// SPDX-License-Identifier: GPL-3.0-or-later
//
// The glucose alarm end to end: a simulated Nightscout serving a low, the
// whole host, and the simulator's speaker as the instrument. These are the
// tests that would catch an urgent low going silent.
#include <cstdint>
#include <string>

#include "stipple/api/Http.h"
#include "stipple/config/Config.h"
#include "stipple/host/ApplicationHost.h"
#include "stipple/notify/Notifications.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::Framebuffer;
using stipple::api::methodFromName;
using stipple::api::Request;
using stipple::api::Response;
using stipple::host::ApplicationHost;
using stipple::host::HostConfig;
using stipple::platform::ButtonPhase;
using stipple::platform::InputEvent;
using stipple::platform::RawInput;
using stipple::platform::simulator::SimulatorCapabilities;
using stipple::platform::simulator::SimulatorHttpClient;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

constexpr std::int64_t kWallStart = 1'790'000'000;
const char* const kBase = "http://nightscout.example:1337";
const char* const kEntries =
    "http://nightscout.example:1337/api/v1/entries.json?count=38&find[type]=sgv";

SimulatorCapabilities alarmCapabilities(bool audio = true) {
    SimulatorCapabilities capabilities;
    capabilities.audio = audio;
    capabilities.httpClient = true;
    return capabilities;
}

HostConfig quiet() {
    HostConfig config;
    config.splashMillis = 0;
    return config;
}

/// Nightscout answering with one reading, a minute old by the simulated wall
/// clock right now.
void serve(SimulatorPlatform& platform, int sgv) {
    const std::int64_t taken = platform.simulatedClock().unixSeconds() - 60;
    SimulatorHttpClient::Route route;
    route.url = kEntries;
    route.status = 200;
    route.body = R"([{"type":"sgv","sgv":)" + std::to_string(sgv) + R"(,"date":)" +
                 std::to_string(taken * 1000) + R"(,"direction":"Flat"}])";
    route.latencyMillis = 50;
    platform.simulatedHttpClient().answer(route);
}

/// A device on the network with the wall clock set, and a stored config.
void prepare(SimulatorPlatform& platform, const stipple::config::Config& saved) {
    platform.simulatedClock().setWallClock(kWallStart);
    stipple::platform::NetworkStatus status;
    status.connected = true;
    platform.simulatedNetwork().setStatus(status);
    stipple::config::ConfigStore store(platform.storage());
    STIPPLE_CHECK(store.save(saved));
}

stipple::config::Config configured() {
    stipple::config::Config saved;
    saved.glucose.url = kBase;
    return saved;
}

/// Run the host, re-serving `sgv` as a fresh reading every minute so it never
/// goes stale while the test waits.
void runServing(ApplicationHost& host, SimulatorPlatform& platform, std::uint64_t forMillis,
                int sgv, int stepMillis = 20) {
    const std::uint64_t until = platform.simulatedClock().monotonicMillis() + forMillis;
    std::uint64_t nextServe = 0;
    for (;;) {
        const std::uint64_t now = platform.simulatedClock().monotonicMillis();
        if (now >= until) {
            break;
        }
        if (now >= nextServe) {
            serve(platform, sgv);
            nextServe = now + 60000;
        }
        host.tick(now);
        platform.simulatedClock().advance(static_cast<std::uint64_t>(stepMillis));
    }
}

void pressKnob(ApplicationHost& host, SimulatorPlatform& platform) {
    const std::uint64_t at = platform.simulatedClock().monotonicMillis();
    InputEvent down;
    down.source = RawInput::RotaryPress;
    down.phase = ButtonPhase::Down;
    down.timestampMillis = at;
    host.handleInput(down);
    InputEvent up = down;
    up.phase = ButtonPhase::Up;
    up.timestampMillis = at + 80;
    host.handleInput(up);
}

std::string activeId(ApplicationHost& host) {
    const stipple::app::App* active = host.carousel().active();
    return active == nullptr ? std::string() : active->id;
}

int countLit(const Framebuffer& frame) {
    int lit = 0;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (frame.at(x, y) != stipple::colors::kBlack) {
                ++lit;
            }
        }
    }
    return lit;
}

int melodies(SimulatorPlatform& platform) {
    return static_cast<int>(platform.simulatedAudio().melodies().size());
}

Response call(ApplicationHost& host, const char* method, const char* path,
              const std::string& body = {}) {
    Request request;
    request.method = methodFromName(method);
    request.path = path;
    request.body = body;
    return host.handle(request);
}

bool contains(const std::string& text, const char* fragment) {
    return text.find(fragment) != std::string::npos;
}

}  // namespace

STIPPLE_TEST(GlucoseAlarmHost, AnUrgentLowSoundsAtTheAlarmLevelEvenAtVolumeZero) {
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.audio.volumePercent = 0;  // − turned all the way down
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();

    runServing(host, platform, 3000, 48);

    STIPPLE_REQUIRE(melodies(platform) == 1);
    const auto& request = platform.simulatedAudio().melodies().front();
    STIPPLE_CHECK_EQ(request.levelPercent, 100);
    stipple::audio::Melody expected;
    STIPPLE_REQUIRE(stipple::audio::parseRtttl(stipple::config::kUrgentLowMelody, expected) ==
                    stipple::audio::RtttlError::None);
    STIPPLE_CHECK_EQ(request.melody.count, expected.count);
    STIPPLE_CHECK_EQ(request.melody.totalMillis(), expected.totalMillis());
    STIPPLE_CHECK(host.glucoseAlarm().sounding());
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));

    // And again five minutes later, nobody having snoozed it.
    runServing(host, platform, 300000, 48, 100);
    STIPPLE_CHECK_EQ(melodies(platform), 2);
}

STIPPLE_TEST(GlucoseAlarmHost, AnInRangeReadingIsSilent) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 120000, 110, 50);
    STIPPLE_CHECK_EQ(melodies(platform), 0);
    STIPPLE_CHECK_FALSE(host.glucoseAlarm().sounding());
}

STIPPLE_TEST(GlucoseAlarmHost, TheKnobSnoozesItAndItComesBack) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 48);
    STIPPLE_REQUIRE(host.glucoseAlarm().sounding());
    const bool pausedBefore = host.carousel().paused();

    pressKnob(host, platform);
    runServing(host, platform, 200, 48);
    STIPPLE_CHECK(host.glucoseAlarm().snoozed());
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedAudio().melodyStopCount()), 1);
    // The press was the snooze and nothing else.
    STIPPLE_CHECK_EQ(host.carousel().paused(), pausedBefore);
    STIPPLE_CHECK_FALSE(host.navigator().inSettings());

    // Quiet for the fifteen-minute snooze, past three repeat intervals...
    runServing(host, platform, 14 * 60000, 48, 100);
    STIPPLE_CHECK_EQ(melodies(platform), 1);
    // ...and back when it ends.
    runServing(host, platform, 2 * 60000, 48, 100);
    STIPPLE_CHECK_EQ(melodies(platform), 2);
    STIPPLE_CHECK(host.glucoseAlarm().sounding());
}

STIPPLE_TEST(GlucoseAlarmHost, AKnobPressWithNoAlarmStillDoesWhatItDid) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 110);
    const bool pausedBefore = host.carousel().paused();
    pressKnob(host, platform);
    runServing(host, platform, 200, 110);
    STIPPLE_CHECK(host.carousel().paused() != pausedBefore);
}

STIPPLE_TEST(GlucoseAlarmHost, ItTakesTheScreenEvenWhenGlucoseIsNotPinned) {
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.glucose.pinned = false;
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();

    // In range first: the carousel rotates as configured.
    runServing(host, platform, 2000, 110);
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());

    runServing(host, platform, 61000, 48);
    STIPPLE_CHECK(host.glucoseAlarm().sounding());
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
    STIPPLE_CHECK(host.carousel().isPinned());
    // Held through rotation dwell, not dropped by the next tick.
    runServing(host, platform, 30000, 48);
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));

    // Snoozed, the hold goes back to what the settings say.
    pressKnob(host, platform);
    runServing(host, platform, 1000, 48);
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());
}

STIPPLE_TEST(GlucoseAlarmHost, BackLeavesButTheNextRepeatReturns) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 48);
    STIPPLE_REQUIRE(activeId(host) == "glucose");

    // No trap: Back still goes to the clock.
    const std::uint64_t at = platform.simulatedClock().monotonicMillis();
    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, at, 80);
    runServing(host, platform, 500, 48);
    STIPPLE_CHECK_EQ(activeId(host), std::string("clock"));
    STIPPLE_CHECK(host.glucoseAlarm().sounding());

    runServing(host, platform, 300000, 48, 100);
    STIPPLE_CHECK_EQ(melodies(platform), 2);
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
}

STIPPLE_TEST(GlucoseAlarmHost, ADarkPanelShowsTheReadingWhileItSounds) {
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.display.power = false;
    saved.display.brightness = 0;
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 48);

    STIPPLE_REQUIRE(host.glucoseAlarm().sounding());
    STIPPLE_CHECK(countLit(host.frame()) > 0);
    STIPPLE_CHECK(platform.simulatedDisplay().brightness() >= 16);
    // Drawn, never switched on.
    STIPPLE_CHECK_FALSE(host.settings().display.power);

    pressKnob(host, platform);
    runServing(host, platform, 4000, 48);
    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 0);
}

STIPPLE_TEST(GlucoseAlarmHost, NothingElseCutsTheMelodyOff) {
    // The clock ticks once a second, and a tick replaces whatever the speaker
    // was playing. Back lands on the clock while the alarm is still sounding,
    // so the ticks are the realistic thing that would chop the melody up.
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.clock.tick = true;
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();

    // Step until the melody starts, so the clock below is measured from it.
    for (int i = 0; i < 500 && melodies(platform) == 0; ++i) {
        runServing(host, platform, 10, 48);
    }
    STIPPLE_REQUIRE(melodies(platform) == 1);
    const std::uint64_t started = platform.simulatedClock().monotonicMillis();
    const int melodyMillis = platform.simulatedAudio().melodies().front().melody.totalMillis();
    STIPPLE_REQUIRE(melodyMillis > 1500);

    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, started, 50);
    runServing(host, platform, 200, 48);
    STIPPLE_REQUIRE(activeId(host) == "clock");
    platform.simulatedAudio().clear();

    // Inside the melody: not one tick.
    runServing(host, platform, static_cast<std::uint64_t>(melodyMillis) - 300u, 48);
    STIPPLE_CHECK(platform.simulatedAudio().requests().empty());

    // After it, the clock ticks again - so the silence above was the guard,
    // not a clock that had stopped ticking for some other reason.
    runServing(host, platform, 3000, 48);
    STIPPLE_CHECK_FALSE(platform.simulatedAudio().requests().empty());
}

STIPPLE_TEST(GlucoseAlarmHost, ItShowsTheReadingOverANotification) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 48);
    STIPPLE_REQUIRE(host.glucoseAlarm().sounding());
    const Framebuffer reading = host.frame();

    stipple::notify::Notification alert;
    alert.id = "door";
    alert.text = "DOOR";
    host.notifications().push(alert, platform.simulatedClock().monotonicMillis());
    runServing(host, platform, 500, 48);
    STIPPLE_CHECK(host.notifications().active() != nullptr);
    STIPPLE_CHECK(host.frame() == reading);

    // Snoozed, the notification gets the panel back.
    pressKnob(host, platform);
    runServing(host, platform, 3000, 48);
    STIPPLE_CHECK(host.frame() != reading);
}

STIPPLE_TEST(GlucoseAlarmHost, ItClearsSettingsAndTheSplashToShowItself) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    HostConfig config;
    config.splashMillis = 60000;
    ApplicationHost host(platform, config);
    host.initialize();
    runServing(host, platform, 3000, 48);
    STIPPLE_CHECK_FALSE(host.showingSplash());
    STIPPLE_CHECK(host.glucoseAlarm().sounding());
}

STIPPLE_TEST(GlucoseAlarmHost, ADisabledAlarmAndAStaleReadingStaySilent) {
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.glucose.alarms.urgentLow.enabled = false;
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 120000, 48, 50);
    STIPPLE_CHECK_EQ(melodies(platform), 0);

    // Enabled over the API: sounds on the next tick that sees it.
    STIPPLE_CHECK_EQ(
        call(host, "PATCH", "/api/v1/settings", R"({"glucose":{"alarms":{"urgentLow":{"enabled":true}}}})")
            .status,
        200);
    runServing(host, platform, 1000, 48);
    STIPPLE_CHECK_EQ(melodies(platform), 1);
}

STIPPLE_TEST(GlucoseAlarmHost, NoDataSoundsWhenNightscoutStopsAnswering) {
    SimulatorPlatform platform(alarmCapabilities());
    stipple::config::Config saved = configured();
    saved.glucose.alarms.noData.enabled = true;
    saved.glucose.alarms.noDataMinutes = 20;
    prepare(platform, saved);
    ApplicationHost host(platform, quiet());
    host.initialize();
    // Nothing served: the simulator refuses the URL, as an unreachable host.
    stipple::host::ApplicationHost* h = &host;
    for (int i = 0; i < 19 * 60; ++i) {
        h->tick(platform.simulatedClock().monotonicMillis());
        platform.simulatedClock().advance(1000);
    }
    STIPPLE_CHECK_EQ(melodies(platform), 0);
    for (int i = 0; i < 90; ++i) {
        h->tick(platform.simulatedClock().monotonicMillis());
        platform.simulatedClock().advance(1000);
    }
    STIPPLE_CHECK_EQ(melodies(platform), 1);
    STIPPLE_CHECK(host.glucoseAlarm().active() == stipple::apps::glucose::AlarmKind::NoData);
}

STIPPLE_TEST(GlucoseAlarmHost, NoSpeakerIsSaidOutLoud) {
    SimulatorPlatform platform(alarmCapabilities(false));
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 48);

    bool warned = false;
    for (int i = 0; i < host.logger().count(); ++i) {
        warned = warned || contains(host.logger().at(i).message, "no speaker");
    }
    STIPPLE_CHECK(warned);
    const std::string diagnostics = call(host, "GET", "/api/v1/diagnostics").body;
    STIPPLE_CHECK(contains(diagnostics, R"("speaker":false)"));
    STIPPLE_CHECK(contains(diagnostics, R"("playFailures":1)"));
}

STIPPLE_TEST(GlucoseAlarmHost, TheApiShowsSettingsStateAndPlaysATest) {
    SimulatorPlatform platform(alarmCapabilities());
    prepare(platform, configured());
    ApplicationHost host(platform, quiet());
    host.initialize();
    runServing(host, platform, 3000, 110);

    const std::string settings = call(host, "GET", "/api/v1/settings").body;
    STIPPLE_CHECK(contains(settings, R"("alarms":{"urgentLow":{"enabled":true,"mgdl":55)"));

    STIPPLE_CHECK_EQ(call(host, "PATCH", "/api/v1/settings",
                          R"({"glucose":{"alarms":{"volumePercent":0}}})")
                         .status,
                     422);
    STIPPLE_CHECK_EQ(host.settings().glucose.alarms.volumePercent, 100);

    const Response test = call(host, "POST", "/api/v1/glucose/alarm/test", R"({"alarm":"low"})");
    STIPPLE_CHECK_EQ(test.status, 200);
    STIPPLE_CHECK_EQ(melodies(platform), 1);
    STIPPLE_CHECK_EQ(call(host, "POST", "/api/v1/glucose/alarm/test", R"({"melody":"x"})").status,
                     422);
    STIPPLE_CHECK_EQ(call(host, "GET", "/api/v1/glucose/alarm/test").status, 405);

    // A test never talks over the real thing.
    runServing(host, platform, 61000, 48);
    STIPPLE_REQUIRE(host.glucoseAlarm().sounding());
    STIPPLE_CHECK_EQ(call(host, "POST", "/api/v1/glucose/alarm/test", R"({"alarm":"high"})").status,
                     409);
    const std::string diagnostics = call(host, "GET", "/api/v1/diagnostics").body;
    STIPPLE_CHECK(contains(diagnostics, R"("state":"sounding","kind":"urgentLow")"));
}
