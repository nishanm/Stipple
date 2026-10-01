// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/host/ApplicationHost.h"

#include <cstdio>
#include <string>

#include "stipple/apps/VisualizerApp.h"
#include "stipple/asset/IconStore.h"
#include "stipple/graphics/Canvas.h"
#include "stipple/time/Timezone.h"
#include "stipple/input/Navigator.h"
#include "stipple/input/Rescue.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "stipple/core/Base64.h"
#include "support/TestFramework.h"

using stipple::Framebuffer;
using stipple::app::AppSource;
using stipple::app::Builtin;
using stipple::host::ApplicationHost;
using stipple::host::BootMode;
using stipple::host::HostConfig;
using stipple::platform::ButtonPhase;
using stipple::platform::InputEvent;
using stipple::platform::RawInput;
using stipple::platform::simulator::SimulatorPlatform;
namespace colors = stipple::colors;

namespace {

int countLit(const Framebuffer& framebuffer) {
    int count = 0;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (framebuffer.at(x, y) != colors::kBlack) {
                ++count;
            }
        }
    }
    return count;
}

HostConfig quietConfig() {
    HostConfig config;
    config.splashMillis = 0;  // most tests are not about the splash
    return config;
}

/// Drive the host over a span of simulated time.
void run(ApplicationHost& host, SimulatorPlatform& platform, std::uint64_t untilMillis,
         int stepMillis = 10) {
    std::uint64_t now = platform.simulatedClock().monotonicMillis();
    while (now < untilMillis) {
        host.tick(now);
        platform.simulatedClock().advance(static_cast<std::uint64_t>(stepMillis));
        now = platform.simulatedClock().monotonicMillis();
    }
}

/// Hold the knob, which is the way into settings and back out (ADR 0017).
void holdKnob(ApplicationHost& host, SimulatorPlatform& platform, std::uint64_t atMillis) {
    platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, atMillis, 900);
    host.tick(atMillis + 1000);
}

/// Turn the knob until the named setting is selected, or give up rather than
/// spin forever if it is not reachable.
bool selectSetting(ApplicationHost& host, SimulatorPlatform& platform,
                   stipple::input::SettingSlot slot, std::uint64_t atMillis) {
    for (int i = 0; i < 8; ++i) {
        if (host.navigator().current() == slot) {
            return true;
        }
        platform.simulatedInput().rotate(true, atMillis + static_cast<std::uint64_t>(i) * 200u);
        host.tick(atMillis + static_cast<std::uint64_t>(i) * 200u + 100u);
    }
    return host.navigator().current() == slot;
}

bool logContains(const ApplicationHost& host, const char* fragment) {
    for (int i = 0; i < host.logger().count(); ++i) {
        if (std::string(host.logger().at(i).message).find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// --- startup -----------------------------------------------------------------

STIPPLE_TEST(Host, InitialisesAndInstallsTheClock) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());

    STIPPLE_CHECK(host.initialize());
    STIPPLE_CHECK(host.initialized());
    STIPPLE_CHECK(host.bootMode() == BootMode::Normal);

    const stipple::app::App* clock = host.apps().find("clock");
    STIPPLE_CHECK(clock != nullptr);
    STIPPLE_CHECK(clock->builtin == Builtin::Clock);
    STIPPLE_CHECK(clock->source == AppSource::System);
}

STIPPLE_TEST(Host, AdoptsThePanelsFrameInterval) {
    // The panel's floor must win over any configured target rate.
    SimulatorPlatform platform;
    HostConfig config = quietConfig();
    config.frame.targetFps = 120;
    ApplicationHost host(platform, config);
    host.initialize();

    STIPPLE_CHECK(host.scheduler().intervalMillis() >=
                 platform.display().minimumFrameIntervalMillis());
}

STIPPLE_TEST(Host, LogsWhatThisPlatformCannotDo) {
    // A device that cannot be reached should say why rather than look broken.
    stipple::platform::simulator::SimulatorCapabilities none;
    none.network = false;
    SimulatorPlatform platform(none);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK(logContains(host, "no network interface"));
    STIPPLE_CHECK(logContains(host, "no HTTP transport"));
}

STIPPLE_TEST(Host, AppliesStoredBrightnessAtBoot) {
    SimulatorPlatform platform;
    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.display.brightness = 42;
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), 42);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.display().brightness()), 42);
}

STIPPLE_TEST(Host, StoredUtcOffsetReachesTheClockFace) {
    // The gap that let a whole setting do nothing.
    //
    // clock.utcOffsetSeconds was validated by the API, persisted by the config
    // store and read back correctly, while renderClock took its offset from
    // ISystemClock instead - so every one of those passed and the panel never
    // moved. Nothing asserted the path from stored setting to rendered frame,
    // which is the only assertion that would have caught it.
    //
    // 7200 is Amsterdam in summer, which is where it was found.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.clock.utcOffsetSeconds = 7200;
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK_EQ(host.settings().clock.utcOffsetSeconds, 7200);
    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 7200);
}

// --- display power -----------------------------------------------------------

STIPPLE_TEST(Host, DisplayPowerOffBlanksThePanel) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.display.power = false;
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 1000);

    // Presented, not merely skipped: the panel has to actually go dark rather
    // than hold whatever happened to be on it.
    STIPPLE_CHECK(platform.simulatedDisplay().presentCount() > 0);
    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);
}

STIPPLE_TEST(Host, SwitchingTheDisplayOffTakesEffectImmediately) {
    // The bug this guards: with dirty rendering, a panel switched off mid-minute
    // would otherwise stay lit until the clock next changed.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 1000);
    STIPPLE_CHECK(countLit(host.frame()) > 0);

    host.settings().display.power = false;
    run(host, platform, 1200);

    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);
}

STIPPLE_TEST(Host, SwitchingTheDisplayBackOnRestoresIt) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.power = false;
    run(host, platform, 1000);
    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);

    host.settings().display.power = true;
    run(host, platform, 2000);

    STIPPLE_CHECK(countLit(host.frame()) > 0);
}

STIPPLE_TEST(Host, ADarkPanelOnlyRedrawsForThePeriodicRefresh) {
    // An off switch that still rendered black at 30 FPS would defeat its own
    // purpose. What should remain is the self-healing refresh and nothing else,
    // so this is asserted against the refresh cadence rather than against zero.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    HostConfig config = quietConfig();
    config.frame.periodicRefreshMillis = 5000;
    ApplicationHost host(platform, config);
    host.initialize();

    host.settings().display.power = false;
    run(host, platform, 1000);
    const int settled = static_cast<int>(host.frameStats().rendered);

    const std::uint64_t spanMillis = 20000;
    run(host, platform, 1000 + spanMillis);

    const int drawn = static_cast<int>(host.frameStats().rendered) - settled;
    const int refreshes = static_cast<int>(spanMillis / config.frame.periodicRefreshMillis);
    STIPPLE_CHECK(drawn <= refreshes + 1);

    // And the comparison that gives the number meaning: a lit clock over the
    // same span redraws many times more often.
    SimulatorPlatform lit;
    lit.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost litHost(lit, config);
    litHost.initialize();
    run(litHost, lit, 1000);
    const int litSettled = static_cast<int>(litHost.frameStats().rendered);
    run(litHost, lit, 1000 + spanMillis);

    STIPPLE_CHECK(static_cast<int>(litHost.frameStats().rendered) - litSettled > drawn);
}

STIPPLE_TEST(Host, TimeKeepsRunningWhileTheDisplayIsOff) {
    // Switching the panel back on should show the current moment, not resume
    // where it left off.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().display.power = false;
    run(host, platform, 500);

    platform.simulatedClock().setWallClock(1'700'003'600);  // an hour later
    run(host, platform, 1500);
    host.settings().display.power = true;
    run(host, platform, 3000);

    STIPPLE_CHECK(countLit(host.frame()) > 0);
}

// --- volume and brightness from the buttons ----------------------------------

STIPPLE_TEST(Host, TappingPlusAndMinusChangesVolumeWhereThereIsASpeaker) {
    // These used to tap volume on hardware reporting no audio output, so they
    // did nothing whatsoever. Volume is back on them now that there is a
    // speaker behind it, which is where it belongs on a device that makes
    // noise: brightness is set once, volume is reached for.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const int begin = static_cast<int>(host.settings().audio.volumePercent);
    const int step = host.inputMapper().config().volumeStepPercent;

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 50);
    host.tick(200);
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), begin + step);

    platform.simulatedInput().pressAndRelease(RawInput::KeyMinus, 300, 50);
    host.tick(400);
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), begin);
}

STIPPLE_TEST(Host, WithNoSpeakerTheSameTapReachesBrightnessInstead) {
    // The buttons must not go dead again on hardware without audio. The
    // control still means "adjust the thing"; what the thing is depends on
    // what the device can actually do.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = false;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const int begin = static_cast<int>(host.settings().display.brightness);
    const int step = host.inputMapper().config().brightnessStep;

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 50);
    host.tick(200);
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), begin + step);
}

STIPPLE_TEST(Host, HoldingPlusReachesBrightnessWithoutTouchingVolume) {
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const int begin = static_cast<int>(host.settings().display.brightness);
    const int volumeBefore = static_cast<int>(host.settings().audio.volumePercent);
    const int step = host.inputMapper().config().brightnessStep;

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 900);
    host.tick(1200);

    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), begin + step);
    // And holding must not also move the thing a tap would have moved.
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), volumeBefore);
}
STIPPLE_TEST(Host, BrightnessFromTheButtonsReachesThePanel) {
    // Changing the stored setting without telling the display would look
    // exactly like a working control and do nothing at all.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    // Held, because a tap reaches volume on a platform with a speaker.
    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 900);
    host.tick(1200);

    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()),
                    static_cast<int>(host.settings().display.brightness));
}

STIPPLE_TEST(Host, VolumeReachesTheSpeakerThroughSettings) {
    // Volume is no longer on a button; it lives in settings, where a device
    // with no speaker can decline to offer it at all. The plumbing still has to
    // work, so this drives the real path: hold the knob, turn to VOL, press +.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().audio.volumePercent = 40;

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());
    STIPPLE_REQUIRE(selectSetting(host, platform, stipple::input::SettingSlot::Volume, 2000));

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 4000, 50);
    host.tick(4100);

    const int step = host.inputMapper().config().volumeStepPercent;
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), 40 + step);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedAudio().volume()),
                    static_cast<int>(stipple::config::volumeToByte(
                        static_cast<std::uint8_t>(40 + step))));
}

STIPPLE_TEST(Host, ADeviceWithNoSpeakerDoesNotOfferVolume) {
    // ADR 0013 on a panel this size: the honest way to show an absent
    // capability is not to offer the control, rather than to offer one that
    // silently does nothing. This is the defect that put volume on the buttons
    // and left them dead.
    // The simulator claims audio by default, which is how the old dead
    // bindings survived: every test that pressed those buttons had a speaker,
    // and the device does not.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = false;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    STIPPLE_CHECK_FALSE(host.navigator().available(stipple::input::SettingSlot::Volume));
    STIPPLE_CHECK_FALSE(selectSetting(host, platform, stipple::input::SettingSlot::Volume, 2000));
}

STIPPLE_TEST(Host, BrightnessStopsAtTheEnds) {
    // Holding a button against the end of the range must not wrap around: a
    // panel that goes from fully dark to fully bright on one more press reads
    // as a fault.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    // Held throughout: a tap reaches volume where there is a speaker.
    for (int i = 0; i < 40; ++i) {
        const std::uint64_t at = static_cast<std::uint64_t>(i) * 1000u + 100u;
        platform.simulatedInput().pressAndRelease(RawInput::KeyMinus, at, 900);
        host.tick(at + 950);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), 0);

    for (int i = 0; i < 40; ++i) {
        const std::uint64_t at = 60000u + static_cast<std::uint64_t>(i) * 1000u;
        platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, at, 900);
        host.tick(at + 950);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), 255);
}

STIPPLE_TEST(Host, HoldingPlusAndMinusChangesBrightness) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const int start = static_cast<int>(host.settings().display.brightness);
    const int step = host.inputMapper().config().brightnessStep;

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 900);
    host.tick(1100);

    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), start + step);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.display().brightness()), start + step);
}

STIPPLE_TEST(Host, TurningBrightnessUpWakesADarkPanel) {
    // Otherwise the button appears to do nothing on a panel that is switched
    // off, which reads as broken hardware.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.power = false;
    run(host, platform, 1000);
    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 1100, 900);
    run(host, platform, 4000);

    STIPPLE_CHECK(host.settings().display.power);
    STIPPLE_CHECK(countLit(host.frame()) > 0);
}

STIPPLE_TEST(Host, VolumeIsIgnoredWithoutASpeaker) {
    // An absent capability is reported, not faked.
    stipple::platform::simulator::SimulatorCapabilities none;
    none.audio = false;
    SimulatorPlatform platform(none);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    const int start = static_cast<int>(host.settings().audio.volumePercent);

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 100, 50);
    host.tick(200);

    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), start);
    STIPPLE_CHECK(logContains(host, "no audio output"));
}

// --- clock settings reach the renderer ---------------------------------------

STIPPLE_TEST(Host, StoredClockSettingsBecomeTheRenderedStyle) {
    SimulatorPlatform platform;
    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.clock.theme = "weekday";
    saved.clock.twentyFourHour = false;
    saved.clock.leadingZero = false;
    saved.clock.showAmPm = true;
    saved.clock.color = 0xFF8800u;
    saved.clock.accentColor = 0x00FF00u;
    saved.clock.dateColor = 0xFF00FFu;
    saved.clock.dateOrder = "monthDayYear";
    saved.clock.dateSeparator = "slash";
    saved.clock.dateYear = "fourDigit";
    saved.clock.blinkPeriodMillis = 0;
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const stipple::apps::ClockStyle style = host.clockStyle();
    STIPPLE_CHECK(style.theme == stipple::apps::ClockTheme::Weekday);
    STIPPLE_CHECK_FALSE(style.twentyFourHour);
    STIPPLE_CHECK_FALSE(style.leadingZero);
    STIPPLE_CHECK(style.showAmPm);
    STIPPLE_CHECK(style.color == stipple::rgb(255, 136, 0));
    STIPPLE_CHECK(style.accentColor == stipple::rgb(0, 255, 0));
    STIPPLE_CHECK(style.dateColor == stipple::rgb(255, 0, 255));
    STIPPLE_CHECK(style.dateOrder == stipple::apps::DateOrder::MonthDayYear);
    STIPPLE_CHECK(style.dateSeparator == stipple::apps::DateSeparator::Slash);
    STIPPLE_CHECK(style.dateYear == stipple::apps::DateYear::FourDigit);
    STIPPLE_CHECK_EQ(static_cast<int>(style.blinkPeriodMillis), 0);
}

STIPPLE_TEST(Host, UnknownClockSettingNamesFallBackInsteadOfFailing) {
    // A config written by a newer build can name a face this one does not have.
    // Degrading to the default beats refusing to show a clock at all.
    SimulatorPlatform platform;
    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.clock.theme = "holographic";
    saved.clock.dateOrder = "stardate";
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    const stipple::apps::ClockStyle style = host.clockStyle();
    STIPPLE_CHECK(style.theme == stipple::apps::ClockTheme::Minimal);
    STIPPLE_CHECK(style.dateOrder == stipple::apps::DateOrder::DayMonthYear);
}

// --- the loop ----------------------------------------------------------------

STIPPLE_TEST(Host, RendersAndPresentsFrames) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    run(host, platform, 1000);

    STIPPLE_CHECK(host.frameStats().rendered > 0);
    STIPPLE_CHECK(platform.simulatedDisplay().presentCount() > 0);
}

STIPPLE_TEST(Host, StaticContentDoesNotRedrawEveryFrame) {
    // The clock's colon blinks twice a second; it must not cost 30 renders a
    // second to do that.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    run(host, platform, 10000);

    STIPPLE_CHECK(host.frameStats().skipped > host.frameStats().rendered);
}

STIPPLE_TEST(Host, TickReportsShutdown) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK(host.tick(0));
    host.shutdown();
    STIPPLE_CHECK_FALSE(host.tick(10));
}

STIPPLE_TEST(Host, NextDueLetsTheCallerSleep) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.tick(0);

    STIPPLE_CHECK(host.nextDueMillis(0) > 0);
}

// --- the clock face ----------------------------------------------------------

STIPPLE_TEST(Host, ShowsPlaceholderUntilTheWallClockIsSet) {
    // A device that boots before NTP and confidently shows 01:00 is worse than
    // one that admits it does not know.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    const int withoutTime = countLit(host.frame());
    STIPPLE_CHECK(withoutTime > 0);  // "--:--" is drawn, not a blank panel

    platform.simulatedClock().setWallClock(1'700'000'000);
    host.scheduler().invalidate();
    run(host, platform, 400);

    STIPPLE_CHECK(countLit(host.frame()) != withoutTime);
}

// --- splash ------------------------------------------------------------------

STIPPLE_TEST(Host, ShowsASplashBeforeTheClock) {
    SimulatorPlatform platform;
    stipple::platform::NetworkStatus online;
    online.connected = true;
    online.ipv4 = "192.168.1.42";
    platform.simulatedNetwork().setStatus(online);

    HostConfig config;
    config.splashMillis = 3000;
    ApplicationHost host(platform, config);
    host.initialize();

    host.tick(0);
    STIPPLE_CHECK(host.showingSplash());
    STIPPLE_CHECK(countLit(host.frame()) > 0);

    run(host, platform, 3100);
    STIPPLE_CHECK_FALSE(host.showingSplash());
}

STIPPLE_TEST(Host, SplashScrollsSoTheAddressIsReadable) {
    // "0.1.0 - 192.168.1.42" is far wider than 52 pixels; truncating it to
    // "192..." would tell the user nothing.
    SimulatorPlatform platform;
    stipple::platform::NetworkStatus online;
    online.connected = true;
    online.ipv4 = "192.168.1.42";
    platform.simulatedNetwork().setStatus(online);

    HostConfig config;
    config.splashMillis = 5000;
    ApplicationHost host(platform, config);
    host.initialize();

    host.tick(0);
    Framebuffer early = host.frame();

    run(host, platform, 2000);
    STIPPLE_CHECK(host.showingSplash());
    STIPPLE_CHECK(host.frame() != early);
}

STIPPLE_TEST(Host, AnyButtonDismissesTheSplash) {
    SimulatorPlatform platform;
    HostConfig config;
    config.splashMillis = 60000;
    ApplicationHost host(platform, config);
    host.initialize();

    host.tick(0);
    STIPPLE_CHECK(host.showingSplash());

    platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 100, 50);
    host.tick(200);

    STIPPLE_CHECK_FALSE(host.showingSplash());
}

STIPPLE_TEST(Host, ThePressThatSkipsTheSplashDoesNothingElse) {
    // The knob press is bound to pause. Tapping it to skip the splash must not
    // also pause the carousel — the user asked to move on, not to stop.
    SimulatorPlatform platform;
    HostConfig config;
    config.splashMillis = 60000;
    ApplicationHost host(platform, config);
    host.initialize();
    host.tick(0);

    platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 100, 50);
    host.tick(200);

    STIPPLE_CHECK_FALSE(host.showingSplash());
    STIPPLE_CHECK_FALSE(host.carousel().paused());

    // The next press behaves normally.
    platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 300, 50);
    host.tick(400);
    STIPPLE_CHECK(host.carousel().paused());
}

STIPPLE_TEST(Host, SplashCanBeDisabled) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.tick(0);

    STIPPLE_CHECK_FALSE(host.showingSplash());
}

// --- anti-brick --------------------------------------------------------------

STIPPLE_TEST(Host, MarksTheBootHealthyOnceItIsClearlyRunning) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    STIPPLE_CHECK_FALSE(host.healthy());

    run(host, platform, 3500);

    STIPPLE_CHECK(host.healthy());

    std::string stored;
    STIPPLE_CHECK(platform.storage().read(ApplicationHost::kBootStateKey, stored));
    STIPPLE_CHECK(stored.find("true") != std::string::npos);
}

STIPPLE_TEST(Host, AStaticScreenStillBecomesHealthy) {
    // Regression guard. Health used to require N rendered frames, but dirty
    // rendering means a static clock face legitimately draws once and stops. A
    // frame count alone would leave such a device permanently unhealthy, so
    // every restart would count as a failure and it would fall into safe mode
    // for ever — the opposite of what the anti-brick mechanism is for.
    SimulatorPlatform platform;
    HostConfig config = quietConfig();
    config.installClockApp = false;  // nothing to animate at all
    ApplicationHost host(platform, config);
    host.initialize();

    run(host, platform, 4000);

    STIPPLE_CHECK(host.frameStats().rendered < 3);  // genuinely static
    STIPPLE_CHECK(host.healthy());
}

STIPPLE_TEST(Host, CountsABootThatNeverRenderedAsAFailure) {
    // Initialising and then dying before any frame is the signature of a boot
    // loop, and it must survive the reboot.
    SimulatorPlatform platform;
    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();  // no ticks: never got to a frame
    }

    ApplicationHost second(platform, quietConfig());
    second.initialize();
    STIPPLE_CHECK_EQ(second.bootRecord().consecutiveFailures, std::uint32_t(1));
    STIPPLE_CHECK(second.bootMode() == BootMode::Normal);  // one failure is not enough
}

STIPPLE_TEST(Host, FallsIntoSafeModeAfterRepeatedFailures) {
    // The anti-brick guarantee: a poisonous config or app cannot leave a clock
    // that has to be opened up to recover.
    SimulatorPlatform platform;

    for (int attempt = 0; attempt < 3; ++attempt) {
        ApplicationHost host(platform, quietConfig());
        host.initialize();  // crashes before rendering, three times over
    }

    ApplicationHost recovered(platform, quietConfig());
    recovered.initialize();

    STIPPLE_CHECK(recovered.bootMode() == BootMode::SafeMode);
    STIPPLE_CHECK(logContains(recovered, "safe mode"));
}

STIPPLE_TEST(Host, SafeModeIgnoresStoredSettingsAndApps) {
    SimulatorPlatform platform;

    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.deviceName = "poisoned";
    saved.display.brightness = 3;
    store.save(saved);

    for (int attempt = 0; attempt < 3; ++attempt) {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
    }

    ApplicationHost recovered(platform, quietConfig());
    recovered.initialize();

    STIPPLE_CHECK(recovered.bootMode() == BootMode::SafeMode);
    STIPPLE_CHECK_EQ(recovered.settings().deviceName, std::string("stipple"));
    STIPPLE_CHECK_EQ(recovered.apps().count(), 0);
}

STIPPLE_TEST(Host, SafeModeStillDrawsSomething) {
    // A blank panel is indistinguishable from a dead device.
    SimulatorPlatform platform;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
    }

    ApplicationHost recovered(platform, quietConfig());
    recovered.initialize();
    run(recovered, platform, 500);

    STIPPLE_CHECK(countLit(recovered.frame()) > 0);
}

STIPPLE_TEST(Host, HealthyBootClearsTheFailureCount) {
    SimulatorPlatform platform;
    {
        ApplicationHost failed(platform, quietConfig());
        failed.initialize();
    }

    ApplicationHost good(platform, quietConfig());
    good.initialize();
    run(good, platform, 3500);
    STIPPLE_CHECK(good.healthy());

    ApplicationHost next(platform, quietConfig());
    next.initialize();
    STIPPLE_CHECK_EQ(next.bootRecord().consecutiveFailures, std::uint32_t(0));
}

STIPPLE_TEST(Host, UnreadableBootRecordDoesNotStopStartup) {
    SimulatorPlatform platform;
    platform.storage().write(ApplicationHost::kBootStateKey, "{ corrupt");

    ApplicationHost host(platform, quietConfig());
    STIPPLE_CHECK(host.initialize());
    STIPPLE_CHECK(host.bootMode() == BootMode::Normal);
}

// --- API wiring --------------------------------------------------------------

STIPPLE_TEST(Host, ServesTheApi) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v1/apps";

    const stipple::api::Response response = host.handle(request);
    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(response.body.find("clock") != std::string::npos);
}

namespace {

/// The header a browser or curl would send.
std::string basicHeader(const std::string& user, const std::string& password) {
    const std::string joined = user + ":" + password;
    return "Basic " + stipple::base64::encode(
                          reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size());
}

}  // namespace

STIPPLE_TEST(Host, ServesEverythingWhenNoPasswordIsSet) {
    // Off by default. A device that demanded a password before it would show
    // a clock would be a worse first five minutes than the risk it removes.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request page;
    page.method = stipple::api::Method::Get;
    page.path = "/";
    STIPPLE_CHECK_EQ(host.handle(page).status, 200);

    stipple::api::Request api;
    api.method = stipple::api::Method::Get;
    api.path = "/api/v1/apps";
    STIPPLE_CHECK_EQ(host.handle(api).status, 200);
}

STIPPLE_TEST(Host, ThePasswordCoversThePageAndTheApiAlike) {
    // One gate, because they are the same server and two schemes would be two
    // things to get wrong (ADR 0018). The page matters as much as the API: it
    // is what somebody uses to change the device.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().web.username = "mark";
    host.settings().web.password = "hunter22";

    const char* paths[] = {"/", "/app.js", "/api/v1/apps", "/api/v1/settings",
                           "/api/v1/health"};
    for (const char* path : paths) {
        stipple::api::Request request;
        request.method = stipple::api::Method::Get;
        request.path = path;
        STIPPLE_CHECK_EQ(host.handle(request).status, 401);

        request.authorization = basicHeader("mark", "hunter22");
        STIPPLE_CHECK(host.handle(request).status != 401);
    }
}

STIPPLE_TEST(Host, ADeniedRequestTellsTheBrowserHowToAsk) {
    // Without WWW-Authenticate a browser shows a bare error page and the
    // person has no way to supply what is missing.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().web.username = "mark";
    host.settings().web.password = "hunter22";

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/";

    const stipple::api::Response denied = host.handle(request);
    STIPPLE_CHECK_EQ(denied.status, 401);
    STIPPLE_CHECK(denied.wwwAuthenticate.find("Basic") != std::string::npos);
    STIPPLE_CHECK(denied.wwwAuthenticate.find("STIPPLE") != std::string::npos);
}

STIPPLE_TEST(Host, AWrongPasswordChangesNothing) {
    // The one that matters. A gate that refuses reads and lets writes through
    // would be worse than no gate, because it would look like one.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().web.username = "mark";
    host.settings().web.password = "hunter22";

    const auto before = host.settings().display.brightness;

    stipple::api::Request write;
    write.method = stipple::api::Method::Patch;
    write.path = "/api/v1/settings";
    write.body = R"({"display":{"brightness":7}})";
    write.authorization = basicHeader("mark", "wrong");

    STIPPLE_CHECK_EQ(host.handle(write).status, 401);
    STIPPLE_CHECK_EQ(host.settings().display.brightness, before);
}

STIPPLE_TEST(Host, ServesTheConfigurationUi) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/";

    const stipple::api::Response response = host.handle(request);
    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(response.contentType.find("text/html") != std::string::npos);
    // The front door is the glucose page; the full Stipple page is behind it.
    STIPPLE_CHECK(response.body.find("Glucose clock") != std::string::npos);
    request.path = "/advanced.html";
    STIPPLE_CHECK(host.handle(request).body.find("STIPPLE") != std::string::npos);
}

STIPPLE_TEST(Host, TheUiNeverShadowsTheApi) {
    // Static files are tried first, so an asset named like an endpoint could
    // otherwise hide it. Paths under /api/ must always reach the API — including
    // unknown ones, which should get the API's explanatory 404 rather than a
    // bare "no such page".
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v2/device";

    const stipple::api::Response response = host.handle(request);
    STIPPLE_CHECK_EQ(response.status, 404);
    STIPPLE_CHECK(response.body.find("/api/v1") != std::string::npos);
}

STIPPLE_TEST(Host, ServesItsOwnLog) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v1/logs";

    const stipple::api::Response response = host.handle(request);
    STIPPLE_CHECK_EQ(response.status, 200);
    // Boot writes several lines, so this is never legitimately empty.
    STIPPLE_CHECK(response.body.find("STIPPLE starting") != std::string::npos);
    STIPPLE_CHECK(response.body.find("totalWritten") != std::string::npos);
}

STIPPLE_TEST(Host, ReadingTheUiIsNotLogged) {
    // The log is 24 entries. A browser fetching three files per page load would
    // push out everything worth seeing.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    const int before = host.logger().count();

    const char* paths[] = {"/", "/app.css", "/app.js"};
    for (const char* path : paths) {
        stipple::api::Request request;
        request.method = stipple::api::Method::Get;
        request.path = path;
        STIPPLE_CHECK_EQ(host.handle(request).status, 200);
    }

    STIPPLE_CHECK_EQ(host.logger().count(), before);
}

STIPPLE_TEST(Host, MqttStaysOffUntilItIsConfigured) {
    // §20: the device must be fully usable without a broker, and must never dial
    // out to one nobody asked it to talk to.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 2000);

    STIPPLE_CHECK(host.mqttService().state() == stipple::platform::MqttState::Disabled);
    STIPPLE_CHECK(platform.simulatedMqtt().published().empty());
}

STIPPLE_TEST(Host, ConfiguringMqttOverTheApiConnectsIt) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    stipple::api::Request request;
    request.method = stipple::api::Method::Patch;
    request.path = "/api/v1/settings";
    request.body = R"({"mqtt":{"enabled":true,"host":"broker.local"}})";
    STIPPLE_CHECK_EQ(host.handle(request).status, 200);

    run(host, platform, 2000);

    STIPPLE_CHECK(host.mqttService().state() == stipple::platform::MqttState::Connected);
    STIPPLE_CHECK(platform.simulatedMqtt().lastOn(host.mqttService().topics().availability) !=
                 nullptr);
}

STIPPLE_TEST(Host, SafeModeStaysOffTheBroker) {
    // Whatever put the device in safe mode might be reachable from a broker, and
    // a boot loop republishing retained state each time is worse than a quiet
    // one.
    SimulatorPlatform platform;

    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.mqtt.enabled = true;
    saved.mqtt.host = "broker.local";
    store.save(saved);

    platform.storage().write(ApplicationHost::kBootStateKey,
                             R"({"consecutiveFailures":5,"lastBootCompleted":false})");

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 2000);

    STIPPLE_CHECK(host.bootMode() == BootMode::SafeMode);
    STIPPLE_CHECK(platform.simulatedMqtt().published().empty());
}

STIPPLE_TEST(Host, ButtonPressesReachTheBroker) {
    SimulatorPlatform platform;

    // Saved rather than poked in after construction: initialize() loads stored
    // settings over whatever is in memory, so anything set beforehand is lost.
    stipple::config::ConfigStore store(platform.storage());
    stipple::config::Config saved;
    saved.mqtt.enabled = true;
    saved.mqtt.host = "broker.local";
    store.save(saved);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 1000);

    platform.simulatedMqtt().clear();
    platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 1100, 50);
    run(host, platform, 1400);

    const stipple::platform::MqttMessage* event =
        platform.simulatedMqtt().lastOn(host.mqttService().topics().button);
    STIPPLE_REQUIRE(event != nullptr);
    STIPPLE_CHECK(std::string(event->payload).find("appAction") != std::string::npos);
}

STIPPLE_TEST(Host, MutatingApiCallsTriggerARedraw) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    // Settle into the steady state where nothing needs redrawing.
    host.scheduler().resetStats();
    run(host, platform, 1000);
    const std::uint32_t before = host.frameStats().rendered;

    stipple::api::Request request;
    request.method = stipple::api::Method::Post;
    request.path = "/api/v1/notifications";
    request.body = R"({"text":"Doorbell"})";
    host.handle(request);

    run(host, platform, 1200);
    STIPPLE_CHECK(host.frameStats().rendered > before);
}

STIPPLE_TEST(Host, NotificationsInterruptTheCarousel) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    const Framebuffer clockFace = host.frame();

    stipple::notify::Notification alert;
    alert.text = "Door";
    alert.durationSeconds = 5;
    host.notifications().push(std::move(alert), platform.simulatedClock().monotonicMillis());

    run(host, platform, 1000);
    STIPPLE_CHECK(host.frame() != clockFace);
}

// --- icon persistence --------------------------------------------------------

STIPPLE_TEST(Host, IconsSurviveAReboot) {
    SimulatorPlatform platform;

    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();

        stipple::asset::Icon icon;
        icon.id = "bell";
        icon.width = 4;
        icon.height = 4;
        icon.frameCount = 2;
        icon.frameMillis = 120;
        icon.hasTransparency = true;
        icon.transparent = colors::kMagenta;
        icon.pixels.assign(32, colors::kYellow);
        host.icons().put(std::move(icon));

        run(host, platform, 3500);  // a tick persists the change
    }

    ApplicationHost rebooted(platform, quietConfig());
    rebooted.initialize();

    const stipple::asset::Icon* restored = rebooted.icons().find("bell");
    STIPPLE_CHECK(restored != nullptr);
    STIPPLE_CHECK_EQ(restored->frameCount, 2);
    STIPPLE_CHECK_EQ(restored->frameMillis, std::uint32_t(120));
    STIPPLE_CHECK(restored->hasTransparency);
}

STIPPLE_TEST(Host, SafeModeDoesNotLoadStoredIcons) {
    // Safe mode ignores everything stored, since stored data is one of the
    // things that could have caused the failures that got us here.
    SimulatorPlatform platform;

    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        stipple::asset::Icon icon;
        icon.id = "x";
        icon.width = 2;
        icon.height = 2;
        icon.frameCount = 1;
        icon.pixels.assign(4, colors::kRed);
        host.icons().put(std::move(icon));
        run(host, platform, 3500);
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        ApplicationHost failing(platform, quietConfig());
        failing.initialize();  // never renders
    }

    ApplicationHost recovered(platform, quietConfig());
    recovered.initialize();
    STIPPLE_CHECK(recovered.bootMode() == BootMode::SafeMode);
    STIPPLE_CHECK_EQ(recovered.icons().count(), 0);
}

STIPPLE_TEST(Host, CorruptStoredIconsDoNotStopStartup) {
    SimulatorPlatform platform;
    platform.storage().write(ApplicationHost::kIconStateKey, "NIC\x01\x7f garbage");

    ApplicationHost host(platform, quietConfig());
    STIPPLE_CHECK(host.initialize());
    STIPPLE_CHECK_EQ(host.icons().count(), 0);

    // The unusable blob is dropped rather than re-read every boot, which would
    // make the failure look intermittent.
    std::string leftover;
    STIPPLE_CHECK_FALSE(platform.storage().read(ApplicationHost::kIconStateKey, leftover));
}

// --- battery -----------------------------------------------------------------

STIPPLE_TEST(Host, BatteryIsOnlyInstalledWhereOneCanBeReported) {
    // A permanent "NO BATT" card in the rotation of a mains-only panel is the
    // carousel's version of a switch that does nothing.
    SimulatorPlatform mainsOnly;
    ApplicationHost without(mainsOnly, quietConfig());
    without.initialize();
    STIPPLE_CHECK(without.apps().find("battery") == nullptr);

    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform battered(capabilities);
    ApplicationHost with(battered, quietConfig());
    with.initialize();
    STIPPLE_CHECK(with.apps().find("battery") != nullptr);
}

STIPPLE_TEST(Host, OneDetentMovesExactlyOneApp) {
    // Rotary acceleration multiplies fast detents up to 5x, which is right for
    // brightness and wrong for a carousel. With three apps installed it made an
    // ordinary turn jump two to five of them and land somewhere that looked
    // random.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    capabilities.microphone = true;
    SimulatorPlatform platform(capabilities);
    platform.simulatedClock().setWallClock(1'700'000'000);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    // clock + stopwatch + visualizer + battery. Read rather than asserted:
    // the claim under test is about detents, and pinning the number here
    // meant adding an app broke a test that has nothing to do with apps.
    const int installed = host.apps().count();
    STIPPLE_CHECK(installed >= 2);

    const stipple::app::App* first = host.carousel().active();
    STIPPLE_CHECK(first != nullptr);
    const std::string startId = first->id;

    const auto detent = [&]() {
        InputEvent tick;
        tick.source = RawInput::RotaryRight;
        tick.phase = ButtonPhase::Tick;
        tick.timestampMillis = platform.simulatedClock().monotonicMillis();
        host.handleInput(tick);
        platform.simulatedClock().advance(20);
    };

    // One detent, one app. This is the actual claim, and it was only being
    // tested implicitly before.
    detent();
    const stipple::app::App* second = host.carousel().active();
    STIPPLE_CHECK(second != nullptr);
    STIPPLE_CHECK(second->id != startId);

    // The rest of a lap, all inside the 120 ms acceleration window so the
    // mapper reports a repeat above one. If acceleration leaked through, this
    // would overshoot and land somewhere else.
    for (int i = 1; i < installed; ++i) {
        detent();
    }

    const stipple::app::App* landed = host.carousel().active();
    STIPPLE_CHECK(landed != nullptr);
    STIPPLE_CHECK_EQ(landed->id, startId);
}

STIPPLE_TEST(Host, AMicrophoneThatNeverDeliversSaysSoRatherThanDrawingSilence) {
    // The exact shape of the TC002 bug. The adapter offers itself as an
    // IMicrophone the moment its serial port opens, then never receives an
    // audio frame. The old render path checked only the pointer, so the app
    // drew its baseline: a flat line across the middle of the panel, which
    // reads as a silent room rather than as a device that cannot hear.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.microphone = true;  // present...
    SimulatorPlatform platform(capabilities);
    // ...but never told to hear anything, which is what the device does.

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    STIPPLE_REQUIRE(host.carousel().activate(ApplicationHost::kVisualizerAppId,
                                            platform.simulatedClock().monotonicMillis()));
    run(host, platform, 400);

    // What a flat line would look like: the baseline spans the full width and
    // is two rows tall, and nothing else is drawn.
    const int flatline = Framebuffer::kWidth * 2;
    STIPPLE_CHECK(countLit(host.frame()) != flatline);

    // And what it should look like instead.
    Framebuffer expected;
    stipple::Canvas canvas(expected);
    stipple::apps::renderNoMicrophone(canvas, colors::kWhite);
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            STIPPLE_CHECK(host.frame().at(x, y) == expected.at(x, y));
        }
    }
}

STIPPLE_TEST(Host, TheVisualizerDrawsSoundOnceItActuallyHearsSomething) {
    // The other half: the honesty check must not have broken the working case.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.microphone = true;
    SimulatorPlatform platform(capabilities);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    STIPPLE_REQUIRE(host.carousel().activate(ApplicationHost::kVisualizerAppId,
                                            platform.simulatedClock().monotonicMillis()));

    platform.simulatedMicrophone().hear(12000);
    run(host, platform, 600);

    Framebuffer noMic;
    stipple::Canvas canvas(noMic);
    stipple::apps::renderNoMicrophone(canvas, colors::kWhite);

    bool differs = false;
    for (int y = 0; y < Framebuffer::kHeight && !differs; ++y) {
        for (int x = 0; x < Framebuffer::kWidth && !differs; ++x) {
            differs = host.frame().at(x, y) != noMic.at(x, y);
        }
    }
    STIPPLE_CHECK(differs);
    STIPPLE_CHECK(countLit(host.frame()) > 0);
}

STIPPLE_TEST(Host, AdjustingVolumeDoesNotReconfigureTheBroker) {
    // A copy of initialize()'s MQTT setup had been spliced into the volume
    // handler. It compiled, because every line of it is a legal statement
    // inside a case block, and no test pressed a volume key while a broker was
    // configured - so every tap of the minus and plus buttons quietly re-ran
    // setContext and configure, and in safe mode wrote "safe mode: MQTT not
    // started" to the ring log on each one.
    //
    // This pins the boundary rather than the symptom: handling an input event
    // is not a configuration event, whatever the action turns out to be.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    const int before = host.logger().count();

    for (int i = 0; i < 8; ++i) {
        InputEvent down;
        down.source = RawInput::KeyPlus;
        down.phase = ButtonPhase::Down;
        down.timestampMillis = platform.simulatedClock().monotonicMillis();
        host.handleInput(down);
        platform.simulatedClock().advance(40);

        InputEvent up;
        up.source = RawInput::KeyPlus;
        up.phase = ButtonPhase::Up;
        up.timestampMillis = platform.simulatedClock().monotonicMillis();
        host.handleInput(up);
        platform.simulatedClock().advance(40);
    }

    // Whatever the button is bound to, pressing it must not talk to MQTT.
    STIPPLE_CHECK_FALSE(logContains(host, "MQTT"));
    STIPPLE_CHECK_FALSE(logContains(host, "safe mode"));
    STIPPLE_CHECK_EQ(host.logger().count(), before);
}

// --- navigating the device itself (ADR 0017) ---------------------------------

STIPPLE_TEST(Host, ThePanelSwitchIsNotOfferedOnThePanel) {
    // It is circular: the control lives on the only surface it switches off,
    // so using it hides the way back. It stays in the settings model and over
    // the API, where a browser can blank the panel and plainly still be used.
    //
    // Turning brightness up already revives a blank panel, which is the
    // gesture someone reaches for anyway - that is the recovery path, and the
    // test below it pins it.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    const bool powerBefore = host.settings().display.power;
    for (int i = 0; i < 12; ++i) {
        platform.simulatedInput().rotate(true, 2000 + static_cast<std::uint64_t>(i) * 200u);
        host.tick(2000 + static_cast<std::uint64_t>(i) * 200u + 100u);
        platform.simulatedInput().pressAndRelease(
            RawInput::KeyMinus, 2100 + static_cast<std::uint64_t>(i) * 200u, 50);
        host.tick(2100 + static_cast<std::uint64_t>(i) * 200u + 100u);
    }
    // Nothing reachable from the knob can have switched the panel off.
    STIPPLE_CHECK_EQ(host.settings().display.power, powerBefore);
}

STIPPLE_TEST(Host, TurningBrightnessUpRevivesABlankedPanel) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    host.settings().display.power = false;
    host.settings().display.brightness = 0;

    // Held, because that is what reaches brightness.
    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 1000, 900);
    host.tick(2000);

    STIPPLE_CHECK(host.settings().display.power);
    STIPPLE_CHECK(host.settings().display.brightness > 0);
}

STIPPLE_TEST(Host, TheCarouselDoesNotAdvanceWhileSettingsAreOpen) {
    // It used to. Every few seconds the timer moved the carousel underneath
    // the menu, which started a transition and slid the settings screen
    // sideways like an app - so settings read as a page in the rotation rather
    // than a mode on top of it. Leaving also landed on whatever app the timer
    // had reached rather than the one the user left.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    const std::string parked = host.carousel().active()->id;

    // Well past any app's dwell time, with activity so settings stay open.
    for (int i = 0; i < 12; ++i) {
        const std::uint64_t at = 2000 + static_cast<std::uint64_t>(i) * 3000u;
        run(host, platform, at, 100);
        platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, at, 50);
        host.tick(at + 50);
    }

    STIPPLE_CHECK_EQ(host.carousel().active()->id, parked);

    // And leaving puts the user back where they were, with a full turn ahead
    // of the app rather than an instant jump to the next one.
    const std::uint64_t leaveAt = platform.simulatedClock().monotonicMillis() + 100;
    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, leaveAt, 50);
    host.tick(leaveAt + 60);
    run(host, platform, leaveAt + 500);
    STIPPLE_CHECK_FALSE(host.navigator().inSettings());
    STIPPLE_CHECK_EQ(host.carousel().active()->id, parked);
}

STIPPLE_TEST(Host, TheKnobMovesBetweenAppsOutsideSettingsAndSettingsInside) {
    // The one rule the whole model rests on: a control means the same thing
    // everywhere, and only what it points at changes.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    const std::string before = host.carousel().active()->id;

    platform.simulatedInput().rotate(true, 500);
    host.tick(600);
    STIPPLE_CHECK(host.carousel().active()->id != before);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    const std::string parked = host.carousel().active()->id;
    const stipple::input::SettingSlot start = host.navigator().current();

    platform.simulatedInput().rotate(true, 2000);
    host.tick(2100);

    // The cursor moved; the carousel did not.
    STIPPLE_CHECK(host.navigator().current() != start);
    STIPPLE_CHECK_EQ(host.carousel().active()->id, parked);
}

STIPPLE_TEST(Host, BackLeavesSettingsBeforeAnythingElse) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, 2000, 50);
    host.tick(2100);
    STIPPLE_CHECK_FALSE(host.navigator().inSettings());
}

STIPPLE_TEST(Host, BackReturnsToTheClockWhenThereIsNothingToLeave) {
    // The last step of "back", and the one that makes it predictable: wherever
    // you are, pressing it enough times lands on the clock.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    STIPPLE_REQUIRE(host.carousel().activate(ApplicationHost::kBatteryAppId, 300));
    STIPPLE_REQUIRE(host.carousel().active()->id != std::string(ApplicationHost::kClockAppId));

    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, 500, 50);
    host.tick(600);

    STIPPLE_CHECK_EQ(host.carousel().active()->id, std::string(ApplicationHost::kClockAppId));
}

STIPPLE_TEST(Host, SettingsCloseThemselvesIfTheUserWalksAway) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(host.navigator().inSettings());

    run(host, platform, 2000 + stipple::input::Navigator::kIdleExitMillis, 100);
    STIPPLE_CHECK_FALSE(host.navigator().inSettings());
    STIPPLE_CHECK(logContains(host, "settings closed after idle"));
}

STIPPLE_TEST(Host, AdjustingBrightnessWhileBrowsingShowsWhatItChanged) {
    // A brightness step is invisible in daylight and at night reads as the
    // panel having glitched. A control with no feedback is indistinguishable
    // from a broken one, which is how volume sat on these buttons doing
    // nothing without anyone noticing.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    const Framebuffer quiet = host.frame();

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 600, 50);
    host.tick(700);
    run(host, platform, 800);

    STIPPLE_CHECK(host.frame() != quiet);
}

STIPPLE_TEST(Host, ChangingVolumePlaysTheNewLevel) {
    // Setting a volume you cannot hear is guesswork, and on a panel showing one
    // number at a time the number is the only other feedback there would be.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    host.settings().audio.volumePercent = 40;
    platform.simulatedAudio().clear();

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(selectSetting(host, platform, stipple::input::SettingSlot::Volume, 2000));

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 4000, 50);
    host.tick(4100);

    STIPPLE_CHECK(!platform.simulatedAudio().requests().empty());
    STIPPLE_CHECK(platform.simulatedAudio().requests().front().isTone);
}

STIPPLE_TEST(Host, TurningVolumeDownToSilenceDoesNotBeep) {
    // A confirmation beep for "silence" is a contradiction, and zero is the one
    // setting where the absence of sound is itself the feedback.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    const int step = host.inputMapper().config().volumeStepPercent;
    host.settings().audio.volumePercent = static_cast<std::uint8_t>(step);

    holdKnob(host, platform, 1000);
    STIPPLE_REQUIRE(selectSetting(host, platform, stipple::input::SettingSlot::Volume, 2000));
    platform.simulatedAudio().clear();

    platform.simulatedInput().pressAndRelease(RawInput::KeyMinus, 4000, 50);
    host.tick(4100);

    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().audio.volumePercent), 0);
    STIPPLE_CHECK(platform.simulatedAudio().requests().empty());
}

// --- sounds the device makes on its own behalf -------------------------------

STIPPLE_TEST(Host, ANotificationAnnouncesItselfOnce) {
    // Once, not once per frame. The sound marks an event arriving, and a
    // notification that holds the panel for five seconds is one event.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    platform.simulatedAudio().clear();

    stipple::notify::Notification alert;
    alert.id = "test";
    alert.text = "HELLO";
    host.notifications().push(alert, platform.simulatedClock().monotonicMillis());

    run(host, platform, 2000);

    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedAudio().requests().size()), 1);
    STIPPLE_CHECK_EQ(platform.simulatedAudio().requests().front().sound, std::string("chime"));
}

STIPPLE_TEST(Host, ANotificationCanNameItsOwnSound) {
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    platform.simulatedAudio().clear();

    stipple::notify::Notification alert;
    alert.id = "test";
    alert.text = "UP";
    alert.sound = "alert";
    host.notifications().push(alert, platform.simulatedClock().monotonicMillis());

    run(host, platform, 2000);

    STIPPLE_REQUIRE(!platform.simulatedAudio().requests().empty());
    STIPPLE_CHECK_EQ(platform.simulatedAudio().requests().front().sound, std::string("alert"));
}

STIPPLE_TEST(Host, NotificationsCanBeSilent) {
    // "none" is a real choice, and the reason the setting is a string rather
    // than a bool: a clock in a bedroom should be able to say nothing.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().notifications.sound = "none";
    run(host, platform, 200);
    platform.simulatedAudio().clear();

    stipple::notify::Notification alert;
    alert.id = "quiet";
    alert.text = "SHH";
    host.notifications().push(alert, platform.simulatedClock().monotonicMillis());
    run(host, platform, 2000);

    STIPPLE_CHECK(platform.simulatedAudio().requests().empty());
}

STIPPLE_TEST(Host, TheClockTicksOnlyWhenAskedTo) {
    // Off by default, and not out of timidity: a sound a device makes once a
    // second without being asked is the easiest way to make somebody unplug it.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);
    platform.simulatedAudio().clear();

    STIPPLE_CHECK_FALSE(host.settings().clock.tick);

    for (int i = 0; i < 5; ++i) {
        platform.simulatedClock().setWallClock(1'700'000'000 + i);
        run(host, platform, 1000 + static_cast<std::uint64_t>(i) * 200u);
    }
    STIPPLE_CHECK(platform.simulatedAudio().requests().empty());
}

STIPPLE_TEST(Host, WhenAskedTheClockAlternatesTickAndTock) {
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().clock.tick = true;
    run(host, platform, 500);
    platform.simulatedAudio().clear();

    for (int i = 1; i <= 4; ++i) {
        platform.simulatedClock().setWallClock(1'700'000'000 + i);
        run(host, platform, 500 + static_cast<std::uint64_t>(i) * 200u);
    }

    const auto& played = platform.simulatedAudio().requests();
    STIPPLE_REQUIRE(played.size() >= 2);
    // Alternating, so a second sounds like a second rather than a repeated blip.
    for (std::size_t i = 1; i < played.size(); ++i) {
        STIPPLE_CHECK(played[i].sound != played[i - 1].sound);
    }
}

STIPPLE_TEST(Host, TheClockDoesNotTickOverANotification) {
    // Ticking under an alarm is being annoying for nobody's benefit.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.audio = true;
    SimulatorPlatform platform(capabilities);
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    host.settings().clock.tick = true;
    host.settings().notifications.sound = "none";
    run(host, platform, 500);

    stipple::notify::Notification alert;
    alert.id = "hold";
    alert.text = "BUSY";
    alert.hold = true;
    host.notifications().push(alert, platform.simulatedClock().monotonicMillis());
    run(host, platform, 700);
    platform.simulatedAudio().clear();

    for (int i = 1; i <= 4; ++i) {
        platform.simulatedClock().setWallClock(1'700'000'000 + i);
        run(host, platform, 700 + static_cast<std::uint64_t>(i) * 200u);
    }
    STIPPLE_CHECK(platform.simulatedAudio().requests().empty());
}

// --- app order ---------------------------------------------------------------

STIPPLE_TEST(Host, AppOrderSurvivesARestart) {
    // Blueprint §12 says the app manager owns ordering and it must never be
    // inferred. An order the user arranged is therefore theirs only if it is
    // written down - otherwise every reboot silently overrules them.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    capabilities.microphone = true;
    SimulatorPlatform platform(capabilities);

    std::string reversedFirst;
    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        run(host, platform, 200);
        STIPPLE_REQUIRE(host.apps().count() >= 3);

        // Move the last app to the front.
        const std::string last = host.apps().at(host.apps().count() - 1)->id;
        STIPPLE_REQUIRE(host.apps().move(last, 0));
        reversedFirst = last;

        run(host, platform, 1000);
        host.shutdown();
    }

    ApplicationHost restarted(platform, quietConfig());
    restarted.initialize();
    run(restarted, platform, 2000);

    STIPPLE_REQUIRE(restarted.apps().count() >= 3);
    STIPPLE_CHECK_EQ(restarted.apps().at(0)->id, reversedFirst);
}

// --- the glucose display as a mode ---------------------------------------------

namespace {

/// A stored configuration with a glucose source, so the app holds the screen.
/// The simulator's HTTP client refuses every URL by default, so the reading
/// stays at no-data; these tests are about the knob, not the source.
stipple::config::Config glucoseConfigured(bool pinned = true) {
    stipple::config::Config saved;
    saved.glucose.url = "http://nightscout.example:1337";
    saved.glucose.pinned = pinned;
    return saved;
}

void seedConfig(SimulatorPlatform& platform, const stipple::config::Config& saved) {
    stipple::config::ConfigStore store(platform.storage());
    STIPPLE_CHECK(store.save(saved));
}

/// One detent, then a tick so the host has processed it.
void detent(ApplicationHost& host, SimulatorPlatform& platform, bool clockwise) {
    InputEvent tick;
    tick.source = clockwise ? RawInput::RotaryRight : RawInput::RotaryLeft;
    tick.phase = ButtonPhase::Tick;
    tick.timestampMillis = platform.simulatedClock().monotonicMillis();
    host.handleInput(tick);
    platform.simulatedClock().advance(200);
    host.tick(platform.simulatedClock().monotonicMillis());
}

std::string activeId(ApplicationHost& host) {
    const stipple::app::App* active = host.carousel().active();
    return active == nullptr ? std::string() : active->id;
}

bool framesEqual(const Framebuffer& a, const Framebuffer& b) {
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (a.at(x, y) != b.at(x, y)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

STIPPLE_TEST(Host, TheGlucoseAppHoldsTheScreenWhenASourceIsConfigured) {
    // A glucose display boots onto glucose and stays there. Without this the
    // carousel carried it away eight seconds after every boot.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
    STIPPLE_CHECK(host.carousel().isPinned());

    run(host, platform, 60000);
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
    STIPPLE_CHECK(host.carousel().isPinned());
}

STIPPLE_TEST(Host, TheKnobStepsGlucoseFacesWhileItHolds) {
    // The knob still moves between things; while the glucose app holds the
    // screen, the things are its faces. One detent, one face, wrapping, and
    // the app never leaves the screen for it.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_REQUIRE(activeId(host) == "glucose");

    detent(host, platform, true);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero-delta"));
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
    STIPPLE_CHECK(host.carousel().isPinned());

    for (int i = 0; i < 4; ++i) {
        detent(host, platform, true);
    }
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));

    detent(host, platform, false);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("big-graph"));
    detent(host, platform, false);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("clock"));
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
}

STIPPLE_TEST(Host, BackLeavesTheGlucoseAppAndTheCarouselComesBackToIt) {
    // No mode is a trap. The middle button keeps its meaning - go back, to
    // the clock - and the pin does not chase the user: the carousel rotates,
    // and when it comes round to glucose again it holds again.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_REQUIRE(activeId(host) == "glucose");

    const std::uint64_t at = platform.simulatedClock().monotonicMillis();
    platform.simulatedInput().pressAndRelease(RawInput::KeyMiddle, at, 100);
    run(host, platform, at + 600);
    STIPPLE_CHECK_EQ(activeId(host), std::string("clock"));
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());

    // Clock and stopwatch dwell eight seconds each by default; glucose is third.
    run(host, platform, at + 40000);
    STIPPLE_CHECK_EQ(activeId(host), std::string("glucose"));
    STIPPLE_CHECK(host.carousel().isPinned());
}

STIPPLE_TEST(Host, AFaceChosenWithTheKnobSurvivesARestart) {
    // Chosen on the device, kept on the device: written once the knob has been
    // still for two seconds, not once per detent.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());
    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        run(host, platform, 200);
        detent(host, platform, true);
        detent(host, platform, true);
        STIPPLE_REQUIRE(host.settings().glucose.face == "hero-graph");
        run(host, platform, platform.simulatedClock().monotonicMillis() + 3000);
        host.shutdown();
    }
    ApplicationHost restarted(platform, quietConfig());
    restarted.initialize();
    run(restarted, platform, platform.simulatedClock().monotonicMillis() + 200);
    STIPPLE_CHECK_EQ(restarted.settings().glucose.face, std::string("hero-graph"));
    STIPPLE_CHECK_EQ(activeId(restarted), std::string("glucose"));
}

STIPPLE_TEST(Host, WithoutASourceTheKnobStillMovesBetweenApps) {
    // Pinned is on by default, but it means nothing until a source exists:
    // a device nobody has pointed at Nightscout keeps Stipple's carousel.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());
    const std::string before = activeId(host);

    detent(host, platform, true);
    STIPPLE_CHECK(activeId(host) != before);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));
}

STIPPLE_TEST(Host, PinnedOffKeepsTheCarouselRotating) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured(false));
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());

    // Rotates through glucose without sticking to it.
    bool sawGlucose = false;
    bool leftGlucose = false;
    for (int i = 0; i < 60; ++i) {
        run(host, platform, platform.simulatedClock().monotonicMillis() + 1000);
        if (activeId(host) == "glucose") {
            sawGlucose = true;
        } else if (sawGlucose) {
            leftGlucose = true;
        }
    }
    STIPPLE_CHECK(sawGlucose);
    STIPPLE_CHECK(leftGlucose);
    STIPPLE_CHECK_FALSE(host.carousel().isPinned());
}

STIPPLE_TEST(Host, SettingsModeStillMovesTheCursorWhileGlucoseHolds) {
    // Holding the knob opens settings whatever is on screen, and inside them
    // the knob moves the cursor - not the face.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    holdKnob(host, platform, platform.simulatedClock().monotonicMillis());
    STIPPLE_REQUIRE(host.navigator().inSettings());
    detent(host, platform, true);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));
    STIPPLE_CHECK(host.navigator().inSettings());
}

STIPPLE_TEST(Host, AFaceChangeIsVisibleEvenWhileTheReadingIsStale) {
    // A stale reading is always drawn as the no-data face, so a detent would
    // change the setting without changing the panel. Feedback is not
    // optional: the readout names the face for a moment instead.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    seedConfig(platform, glucoseConfigured());
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_REQUIRE(activeId(host) == "glucose");
    STIPPLE_REQUIRE(countLit(host.frame()) > 0);
    const Framebuffer noData = host.frame();

    detent(host, platform, true);
    STIPPLE_CHECK_FALSE(framesEqual(host.frame(), noData));

    // And it goes away again, leaving the no-data face as before.
    run(host, platform, platform.simulatedClock().monotonicMillis() + 2000);
    STIPPLE_CHECK(framesEqual(host.frame(), noData));
}

STIPPLE_TEST(Host, ADisabledAppStaysDisabledAcrossARestart) {
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);

    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        run(host, platform, 200);
        STIPPLE_REQUIRE(host.apps().setEnabled(ApplicationHost::kBatteryAppId, false));
        run(host, platform, 1000);
        host.shutdown();
    }

    ApplicationHost restarted(platform, quietConfig());
    restarted.initialize();
    run(restarted, platform, 2000);

    const stipple::app::App* battery = restarted.apps().find(ApplicationHost::kBatteryAppId);
    STIPPLE_REQUIRE(battery != nullptr);
    STIPPLE_CHECK_FALSE(battery->enabled);
}

STIPPLE_TEST(Host, AStoredOrderNamingAnAppThatIsGoneStillBoots) {
    // Firmware changes and integrations stop pushing, so an order that names a
    // missing app is normal rather than exceptional. It must not cost the
    // arrangement of the apps that *are* there, and certainly must not stop
    // the device starting.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);

    // Written through the store, because initialize() loads settings and
    // would overwrite anything set on the host beforehand - which is also
    // exactly how a real device meets a stored order.
    {
        stipple::config::Config stored;
        stipple::config::AppPreference ghost;
        ghost.id = "an-app-that-never-existed";
        stored.apps.order.push_back(ghost);

        stipple::config::AppPreference battery;
        battery.id = std::string(ApplicationHost::kBatteryAppId);
        stored.apps.order.push_back(battery);

        stipple::config::ConfigStore store(platform.storage());
        STIPPLE_REQUIRE(store.save(stored));
    }

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    STIPPLE_CHECK(host.healthy());
    // The real app named after the ghost still took the first position.
    STIPPLE_CHECK_EQ(host.apps().at(0)->id, std::string(ApplicationHost::kBatteryAppId));
}

STIPPLE_TEST(Host, AnAppInstalledSinceTheOrderWasSavedAppearsRatherThanVanishing) {
    // Apps not named by the stored order keep their natural position after the
    // ones that are. Dropping them, or sorting them to the front, would both
    // be the device overruling an arrangement it was only asked to restore.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    capabilities.microphone = true;
    SimulatorPlatform platform(capabilities);

    {
        stipple::config::Config stored;
        stipple::config::AppPreference battery;
        battery.id = std::string(ApplicationHost::kBatteryAppId);
        stored.apps.order.push_back(battery);

        stipple::config::ConfigStore store(platform.storage());
        STIPPLE_REQUIRE(store.save(stored));
    }

    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    STIPPLE_CHECK_EQ(host.apps().at(0)->id, std::string(ApplicationHost::kBatteryAppId));
    // Clock and visualiser were not mentioned, and are still installed.
    STIPPLE_CHECK(host.apps().find(ApplicationHost::kClockAppId) != nullptr);
    STIPPLE_CHECK(host.apps().find(ApplicationHost::kVisualizerAppId) != nullptr);
}

STIPPLE_TEST(Host, ChangingTheAppDurationTakesEffectWithoutARestart) {
    // It was copied into the carousel in initialize() and nowhere else, so
    // changing it over the API updated the stored setting and did nothing at
    // all until the next restart. From outside, a setting that only applies
    // after a reboot and does not say so is a setting that is ignored.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);
    STIPPLE_REQUIRE(host.apps().count() >= 2);

    host.settings().apps.defaultDurationSeconds = 1;
    run(host, platform, 700);

    const std::string before = host.carousel().active()->id;

    // Past one second and well short of two. With only two apps installed, a
    // wider window would advance twice and land back where it started - which
    // is a test that passes for "never moved" and for "moved correctly" alike.
    run(host, platform, 1800);
    STIPPLE_CHECK(host.carousel().active()->id != before);

    // And nowhere near the eight-second default it would have used before.
    STIPPLE_CHECK(host.carousel().config().defaultDurationSeconds == 1);
}

STIPPLE_TEST(Host, ALongerDurationAlsoTakesEffectImmediately) {
    // The other direction, because "it advances sooner" could be satisfied by
    // something ignoring the setting entirely and rotating fast.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);

    host.settings().apps.defaultDurationSeconds = 60;
    run(host, platform, 700);

    const std::string before = host.carousel().active()->id;
    run(host, platform, 20000);
    STIPPLE_CHECK_EQ(host.carousel().active()->id, before);
}

STIPPLE_TEST(Host, ATimezoneRuleBeatsTheStoredOffset) {
    // The bug this replaces: a fixed offset is right for about half the year
    // anywhere that observes daylight saving, and wrong the rest of it.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().clock.utcOffsetSeconds = 0;
    host.settings().clock.timezone = "CET-1CEST,M3.5.0,M10.5.0/3";

    // Deep winter: one hour ahead of UTC.
    platform.simulatedClock().setWallClock(
        stipple::timezone_::daysFromCivil(2026, 1, 15) * 86400);
    run(host, platform, 300);
    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 3600);

    // Deep summer: two.
    platform.simulatedClock().setWallClock(
        stipple::timezone_::daysFromCivil(2026, 7, 15) * 86400);
    run(host, platform, 600);
    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 7200);
}

STIPPLE_TEST(Host, WithoutATimezoneTheStoredOffsetStillApplies) {
    // Every device configured before timezones existed has one of these, and
    // nothing should have to be re-entered to keep working.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().clock.timezone.clear();
    host.settings().clock.utcOffsetSeconds = 5 * 3600;
    platform.simulatedClock().setWallClock(
        stipple::timezone_::daysFromCivil(2026, 7, 15) * 86400);
    run(host, platform, 300);

    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 5 * 3600);
}

STIPPLE_TEST(Host, AnUnparseableRuleFallsBackAndSaysSo) {
    // Falling back silently would leave somebody certain they had set a
    // timezone and puzzled twice a year.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().clock.utcOffsetSeconds = 3 * 3600;
    host.settings().clock.timezone = "not a timezone";
    run(host, platform, 300);

    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 3 * 3600);
    STIPPLE_CHECK(logContains(host, "timezone rule not understood"));
}

STIPPLE_TEST(Host, WithoutAWallClockTheStandardOffsetIsUsed) {
    // Before NTP answers there is no date, so there is no way to know which
    // side of a changeover we are on. Standard time is the honest answer; a
    // coin toss dressed as a summer offset is not.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().clock.timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
    run(host, platform, 300);  // wall clock never set

    STIPPLE_CHECK_FALSE(platform.clock().wallClockValid());
    STIPPLE_CHECK_EQ(host.clockStyle().utcOffsetSeconds, 3600);
}

STIPPLE_TEST(Host, ARestoredOrderReachesTheRegistryWithoutARestart) {
    // Restoring a backup writes settings; the apps on screen are in the
    // registry. Without this the arrangement would come back only on the next
    // reboot - the same defect the app duration had, in a place where it is
    // even less visible.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    capabilities.microphone = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 500);
    STIPPLE_REQUIRE(host.apps().count() >= 3);

    // An arrangement arriving from outside, exactly as a restore delivers it.
    const std::string wanted = host.apps().at(host.apps().count() - 1)->id;
    stipple::config::AppPreference first;
    first.id = wanted;
    host.settings().apps.order.clear();
    host.settings().apps.order.push_back(first);

    run(host, platform, 1000);

    STIPPLE_CHECK_EQ(host.apps().at(0)->id, wanted);
}

STIPPLE_TEST(Host, AnOrderThatAlreadyMatchesIsNotRewritten) {
    // The two directions must not fight. If applying an order counted as a
    // registry change, and writing it back counted as a settings change, the
    // device would save its configuration on every single tick.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 1000);

    const int before = host.logger().count();
    run(host, platform, 6000);

    // No errors, and nothing churning: a save failure would log, and a loop
    // would show up as a stream of them.
    STIPPLE_CHECK_EQ(host.logger().count(), before);
}

// --- overnight dimming --------------------------------------------------------

namespace {

/// A unix timestamp at a given UTC time of day, on a fixed date.
std::uint64_t atUtcHour(int hour, int minute = 0) {
    return static_cast<std::uint64_t>(
        stipple::timezone_::daysFromCivil(2026, 6, 15) * 86400 + hour * 3600 + minute * 60);
}

}  // namespace

STIPPLE_TEST(Host, NightModeDimsInsideItsWindow) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().display.night.enabled = true;
    host.settings().display.night.startMinutes = 22 * 60;
    host.settings().display.night.endMinutes = 7 * 60;
    host.settings().display.night.brightness = 10;

    platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(23)));
    run(host, platform, 400);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 10);

    // And the setting itself is untouched, so the morning gets the panel back
    // exactly as the user left it.
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), 200);
}

STIPPLE_TEST(Host, NightModeWindowWrapsMidnight) {
    // The normal case for a night, and the one a naive "between two times"
    // check reports backwards - bright all night and dim all day.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().display.night.enabled = true;
    host.settings().display.night.startMinutes = 22 * 60;
    host.settings().display.night.endMinutes = 7 * 60;
    host.settings().display.night.brightness = 10;

    const struct { int hour; int expected; } moments[] = {
        {21, 200},  // before it starts
        {22, 10},   // the moment it starts
        {3, 10},    // the small hours
        {6, 10},    // still dim
        {7, 200},   // the moment it ends
        {12, 200},  // midday
    };

    std::uint64_t now = 1000;
    for (const auto& moment : moments) {
        platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(moment.hour)));
        now += 400;
        run(host, platform, now);
        STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()),
                        moment.expected);
    }
}

STIPPLE_TEST(Host, NightModeFollowsLocalTimeNotUtc) {
    // The window is what somebody set looking at their own clock. Applying it
    // in UTC would dim a device in Sydney over lunch.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().clock.timezone = "AEST-10";  // ten hours ahead
    host.settings().display.night.enabled = true;
    host.settings().display.night.startMinutes = 22 * 60;
    host.settings().display.night.endMinutes = 7 * 60;
    host.settings().display.night.brightness = 10;

    // 13:00 UTC is 23:00 local, which is inside the window.
    platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(13)));
    run(host, platform, 400);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 10);

    // 23:00 UTC is 09:00 local, which is not.
    platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(23)));
    run(host, platform, 900);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 200);
}

STIPPLE_TEST(Host, ChangingBrightnessAtNightDoesNotUndim) {
    // Somebody pressing + at 3am wants the panel brighter now, and expects the
    // schedule to still be there tomorrow. The setting moves; what is on the
    // panel stays where the schedule put it until the window ends.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().display.night.enabled = true;
    host.settings().display.night.startMinutes = 22 * 60;
    host.settings().display.night.endMinutes = 7 * 60;
    host.settings().display.night.brightness = 10;

    platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(3)));
    run(host, platform, 400);

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 500, 900);
    run(host, platform, 1600);

    STIPPLE_CHECK(host.settings().display.brightness > 200);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 10);
}

STIPPLE_TEST(Host, WithoutAWallClockNothingIsDimmed) {
    // Dimming because NTP has not answered yet would look exactly like a fault.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().display.night.enabled = true;
    host.settings().display.night.brightness = 10;
    run(host, platform, 400);

    STIPPLE_CHECK_FALSE(platform.clock().wallClockValid());
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 200);
}

STIPPLE_TEST(Host, AWindowOfNoLengthDimsNothing) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    host.settings().display.brightness = 200;
    host.settings().display.night.enabled = true;
    host.settings().display.night.startMinutes = 8 * 60;
    host.settings().display.night.endMinutes = 8 * 60;
    host.settings().display.night.brightness = 10;

    platform.simulatedClock().setWallClock(static_cast<std::int64_t>(atUtcHour(8)));
    run(host, platform, 400);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 200);
}

// --- the way back in (ADR 0018) ----------------------------------------------

namespace {

/// Hold both adjustment buttons for long enough to trigger the rescue.
void holdBothButtons(ApplicationHost& host, SimulatorPlatform& platform,
                     std::uint64_t from, std::uint64_t forMillis) {
    InputEvent down;
    down.phase = ButtonPhase::Down;
    down.timestampMillis = from;
    down.source = RawInput::KeyMinus;
    host.handleInput(down);
    down.source = RawInput::KeyPlus;
    host.handleInput(down);

    run(host, platform, from + forMillis, 100);
}

}  // namespace

STIPPLE_TEST(Host, HoldingBothButtonsClearsTheWayBackIn) {
    // The whole point of ADR 0018's ordering: this exists before anything that
    // can lock somebody out, because it is what makes those safe to build.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 300);

    host.settings().web.username = "admin";
    host.settings().web.password = "forgotten";
    host.settings().network.hotspotRequested = false;

    holdBothButtons(host, platform, 1000, stipple::input::Rescue::kHoldMillis + 500);

    STIPPLE_CHECK(host.settings().web.password.empty());
    STIPPLE_CHECK(host.settings().web.username.empty());
    STIPPLE_CHECK(host.settings().network.hotspotRequested);
    STIPPLE_CHECK(logContains(host, "rescue"));
}

STIPPLE_TEST(Host, TheRescueKeepsAppsAndSettings) {
    // A rescue that costs a week of an integration's work is one people avoid
    // using until it is too late. It clears the way back in and nothing else.
    stipple::platform::simulator::SimulatorCapabilities capabilities;
    capabilities.power = true;
    SimulatorPlatform platform(capabilities);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 300);

    host.settings().web.password = "forgotten";
    host.settings().clock.theme = "calendar";
    host.settings().display.brightness = 42;
    const int appsBefore = host.apps().count();

    holdBothButtons(host, platform, 1000, stipple::input::Rescue::kHoldMillis + 500);

    STIPPLE_CHECK(host.settings().web.password.empty());
    STIPPLE_CHECK_EQ(host.settings().clock.theme, std::string("calendar"));
    STIPPLE_CHECK_EQ(static_cast<int>(host.settings().display.brightness), 42);
    STIPPLE_CHECK_EQ(host.apps().count(), appsBefore);
}

STIPPLE_TEST(Host, TheRescueSurvivesARestart) {
    // Persisted, because if it did not the device would be open now and locked
    // again after the reboot somebody reaches for next - the worst of both and
    // the one outcome nobody could diagnose.
    SimulatorPlatform platform;
    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        run(host, platform, 300);
        host.settings().web.password = "forgotten";
        holdBothButtons(host, platform, 1000, stipple::input::Rescue::kHoldMillis + 500);
        host.shutdown();
    }

    ApplicationHost restarted(platform, quietConfig());
    restarted.initialize();
    run(restarted, platform, 20000);

    STIPPLE_CHECK(restarted.settings().web.password.empty());
    STIPPLE_CHECK(restarted.settings().network.hotspotRequested);
}

STIPPLE_TEST(Host, TheCountdownIsShownEvenWithThePanelOff) {
    // This gesture is reached for precisely when nothing else about the device
    // is behaving. A device that stayed dark and then silently cleared its own
    // password would be indistinguishable from one that crashed.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 300);

    host.settings().display.power = false;
    run(host, platform, 600);
    STIPPLE_CHECK_EQ(countLit(host.frame()), 0);

    InputEvent down;
    down.phase = ButtonPhase::Down;
    down.timestampMillis = 1000;
    down.source = RawInput::KeyMinus;
    host.handleInput(down);
    down.source = RawInput::KeyPlus;
    host.handleInput(down);

    run(host, platform, 2000, 100);

    STIPPLE_CHECK(host.rescue().counting());
    STIPPLE_CHECK(countLit(host.frame()) > 0);
}

STIPPLE_TEST(Host, AnOrdinaryPressDoesNotTriggerTheRescue) {
    // Both buttons are adjustment controls people hold on purpose.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 300);

    host.settings().web.password = "kept";

    platform.simulatedInput().pressAndRelease(RawInput::KeyPlus, 1000, 2000);
    platform.simulatedInput().pressAndRelease(RawInput::KeyMinus, 4000, 2000);
    run(host, platform, 9000, 100);

    STIPPLE_CHECK_EQ(host.settings().web.password, std::string("kept"));
}

STIPPLE_TEST(Host, TheRescueGestureClearsTheLockout) {
    // The way back that does not need the network (ADR 0018). Somebody locked
    // out of a clock has no other route in: USB-C on this device is mass
    // storage, and ADB arrives over the network they cannot reach.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 300);
    host.settings().web.username = "mark";
    host.settings().web.password = "hunter22";

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v1/apps";
    STIPPLE_CHECK_EQ(host.handle(request).status, 401);

    holdBothButtons(host, platform, 1000, stipple::input::Rescue::kHoldMillis + 500);

    STIPPLE_CHECK(host.settings().web.username.empty());
    STIPPLE_CHECK(host.settings().web.password.empty());
    STIPPLE_CHECK_EQ(host.handle(request).status, 200);
}

STIPPLE_TEST(Host, ADeviceWithNothingStoredIsOnItsFirstRun) {
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK(host.firstRun());

    // And says so where the page can see it, because that is what decides
    // whether it opens on the network step or on a live view of a clock
    // showing the wrong time.
    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v1/device";
    const std::string body = host.handle(request).body;
    STIPPLE_CHECK(body.find("\"firstRun\":true") != std::string::npos);
}

STIPPLE_TEST(Host, FirstRunEndsWhenAnythingIsSaved) {
    // A state, not a wizard. There is no "finish setup" button, because a
    // button somebody has to find is a step that can be missed.
    SimulatorPlatform platform;
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    STIPPLE_CHECK(host.firstRun());

    stipple::api::Request patch;
    patch.method = stipple::api::Method::Patch;
    patch.path = "/api/v1/settings";
    patch.body = R"({"display":{"brightness":90}})";
    STIPPLE_CHECK_EQ(host.handle(patch).status, 200);

    STIPPLE_CHECK_FALSE(host.firstRun());

    stipple::api::Request request;
    request.method = stipple::api::Method::Get;
    request.path = "/api/v1/device";
    STIPPLE_CHECK(host.handle(request).body.find("\"firstRun\":false") != std::string::npos);
}

STIPPLE_TEST(Host, ADeviceThatHasBeenConfiguredIsNotOnItsFirstRun) {
    SimulatorPlatform platform;
    {
        ApplicationHost host(platform, quietConfig());
        host.initialize();
        stipple::api::Request patch;
        patch.method = stipple::api::Method::Patch;
        patch.path = "/api/v1/settings";
        patch.body = R"({"display":{"brightness":90}})";
        host.handle(patch);
        host.shutdown();
    }

    ApplicationHost restarted(platform, quietConfig());
    restarted.initialize();
    STIPPLE_CHECK_FALSE(restarted.firstRun());
}

STIPPLE_TEST(Host, CorruptStorageIsNotAFirstRun) {
    // That device *was* configured, and telling its owner it is brand new
    // would be both wrong and the least helpful thing to say while they are
    // working out what happened to their settings.
    SimulatorPlatform platform;
    platform.storage().write(stipple::config::ConfigStore::kPrimaryKey, "{ not json");

    ApplicationHost host(platform, quietConfig());
    host.initialize();

    STIPPLE_CHECK_FALSE(host.firstRun());
}

STIPPLE_TEST(Host, RestoringAnOrderThatActuallyMovesThingsKeepsEachDuration) {
    // The narrow version of a bug found while restoring script apps.
    //
    // applyStoredAppOrder() took a pointer to the app, then called move(),
    // then wrote the duration through that pointer. move() erases and
    // reinserts, so everything between the old and new position shifts and
    // the pointer no longer refers to the same app - the duration landed on
    // whichever one had been shuffled into that slot.
    //
    // It hid for a long time because move() returns early when an app is
    // already in the right place, which is every boot where nothing has
    // changed. This arranges a stored order that genuinely reorders things.
    //
    // Built-in apps, because they are the ones that exist again after a
    // restart: an app pushed over the API lives in RAM and only its position
    // in the order is remembered.
    // Power and the microphone turned on, so the battery and visualizer apps
    // exist. Three apps is the smallest number that shows this: with two, the
    // second pass through the loop happens to overwrite the damage the first
    // one did, and the test would pass against the broken code.
    stipple::platform::simulator::SimulatorCapabilities all;
    all.power = true;
    all.microphone = true;
    SimulatorPlatform platform(all);

    {
        ApplicationHost host(platform, quietConfig());
        STIPPLE_REQUIRE(host.initialize());

        const struct { const char* id; const char* body; } wanted[] = {
            {"clock", R"({"durationSeconds":11})"},
            {"stopwatch", R"({"durationSeconds":22})"},
            {"battery", R"({"durationSeconds":33})"},
        };
        for (const auto& each : wanted) {
            stipple::api::Request patch;
            patch.method = stipple::api::Method::Patch;
            patch.path = std::string("/api/v1/apps/") + each.id;
            patch.body = each.body;
            STIPPLE_REQUIRE(host.handle(patch).status == 200);
        }

        // Reverse them, so restoring has real work to do.
        STIPPLE_REQUIRE(host.apps().move("battery", 0));
        STIPPLE_REQUIRE(host.apps().move("stopwatch", 1));
        host.tick(1000);
        host.tick(2000);
    }

    ApplicationHost host(platform, quietConfig());
    STIPPLE_REQUIRE(host.initialize());

    // Each duration on its own app, not on whichever one moved into its place.
    const struct { const char* id; int seconds; } expected[] = {
        {"clock", 11}, {"stopwatch", 22}, {"battery", 33},
    };
    for (const auto& want : expected) {
        const stipple::app::App* app = host.apps().find(want.id);
        STIPPLE_REQUIRE(app != nullptr);
        STIPPLE_CHECK_EQ(app->durationSeconds, want.seconds);
    }

    STIPPLE_CHECK_EQ(host.apps().indexOf("battery"), 0);
    STIPPLE_CHECK_EQ(host.apps().indexOf("stopwatch"), 1);
}

// --- glucose faces: the set in use, cycling and the schedule ------------------

STIPPLE_TEST(Host, TheKnobMovesOnlyBetweenFacesInUse) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    stipple::config::Config saved = glucoseConfigured();
    saved.glucose.faces = {"hero", "clock"};
    saved.glucose.face = "hero";
    seedConfig(platform, saved);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_REQUIRE(activeId(host) == "glucose");

    detent(host, platform, true);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("clock"));
    detent(host, platform, true);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Hero);
    detent(host, platform, false);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);
}

STIPPLE_TEST(Host, CyclingStepsThroughTheFacesInUseWithoutWritingTheDefault) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    stipple::config::Config saved = glucoseConfigured();
    saved.glucose.faces = {"hero", "hero-graph", "clock"};
    saved.glucose.face = "hero";
    saved.glucose.cycleSeconds = 10;
    seedConfig(platform, saved);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Hero);

    run(host, platform, 10'500);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::HeroGraph);
    run(host, platform, 20'500);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);
    run(host, platform, 30'500);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Hero);
    // The default stays the default: cycling is not a choice to persist.
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));

    // The knob moves the face for now and restarts the interval; the default
    // is still not rewritten.
    detent(host, platform, true);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::HeroGraph);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));
    const std::uint64_t turned = platform.simulatedClock().monotonicMillis();
    run(host, platform, turned + 9'000);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::HeroGraph);
    run(host, platform, turned + 10'500);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);
}

STIPPLE_TEST(Host, TheScheduleSetsFaceAndBrightnessAndTheKnobStillWorks) {
    // 1'700'000'000 is 22:13 UTC, inside the evening row.
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    stipple::config::Config saved = glucoseConfigured();
    saved.glucose.face = "hero";
    saved.glucose.cycleSeconds = 10;  // the schedule wins over cycling
    saved.glucose.schedule.enabled = true;
    saved.glucose.schedule.rows = {{7 * 60, "hero", 200}, {22 * 60, "clock", 8}};
    seedConfig(platform, saved);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);

    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 8);

    run(host, platform, 30'000);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::Clock);

    // The knob changes the face in between rows, and the schedule leaves it.
    detent(host, platform, true);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::BigGraph);
    run(host, platform, platform.simulatedClock().monotonicMillis() + 60'000);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::BigGraph);
    STIPPLE_CHECK_EQ(host.settings().glucose.face, std::string("hero"));
}

STIPPLE_TEST(Host, ARowWithoutBrightnessLeavesThePanelSetting) {
    SimulatorPlatform platform;
    platform.simulatedClock().setWallClock(1'700'000'000);
    stipple::config::Config saved = glucoseConfigured();
    saved.display.brightness = 99;
    saved.glucose.schedule.enabled = true;
    saved.glucose.schedule.rows = {{22 * 60, "big-graph", -1}};
    seedConfig(platform, saved);
    ApplicationHost host(platform, quietConfig());
    host.initialize();
    run(host, platform, 200);
    STIPPLE_CHECK(host.glucoseFaceShown() == stipple::apps::GlucoseFace::BigGraph);
    STIPPLE_CHECK_EQ(static_cast<int>(platform.simulatedDisplay().brightness()), 99);
}
