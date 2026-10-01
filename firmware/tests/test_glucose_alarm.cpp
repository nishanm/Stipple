// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <string>
#include <vector>

#include "stipple/apps/GlucoseAlarm.h"
#include "stipple/config/Config.h"
#include "stipple/config/GlucoseAlarmSettings.h"
#include "stipple/json/Json.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::apps::glucose::AlarmInputs;
using stipple::apps::glucose::AlarmKind;
using stipple::apps::glucose::GlucoseAlarm;
using stipple::apps::glucose::insideWindow;
using stipple::apps::glucose::localDayAndMinute;
using stipple::config::AlertWindow;
using stipple::config::GlucoseAlarmSettings;

namespace {

constexpr std::uint64_t kMinute = 60000;

int kindOf(AlarmKind kind) { return static_cast<int>(kind); }

/// A fresh reading at `sgv`, at `now`.
AlarmInputs reading(int sgv, std::uint64_t now) {
    AlarmInputs in;
    in.nowMillis = now;
    in.configured = true;
    in.fresh = true;
    in.sgv = sgv;
    in.dataAgeMinutes = 1;
    return in;
}

/// Every alarm on, at the nightscout-clock defaults.
GlucoseAlarmSettings allOn() {
    GlucoseAlarmSettings settings;
    settings.urgentLow.enabled = true;
    settings.low.enabled = true;
    settings.high.enabled = true;
    return settings;
}

AlarmKind classifyOnce(const GlucoseAlarmSettings& settings, int sgv) {
    GlucoseAlarm alarm;
    alarm.configure(settings);
    return alarm.tick(reading(sgv, 1000)).kind;
}

/// Parse `text` and apply it as a PATCH would.
bool apply(const std::string& text, GlucoseAlarmSettings& settings, std::string& error) {
    stipple::json::Token tokens[512];
    stipple::json::Document document(tokens, 512);
    if (document.parse(text) != stipple::json::Error::None) {
        error = "bad json in test";
        return false;
    }
    return stipple::config::applyAlarmSettings(document.root(), settings, error);
}

}  // namespace

STIPPLE_TEST(GlucoseAlarm, RangesMatchTheClocks) {
    const GlucoseAlarmSettings settings = allOn();
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 40)), kindOf(AlarmKind::UrgentLow));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 55)), kindOf(AlarmKind::UrgentLow));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 56)), kindOf(AlarmKind::Low));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 69)), kindOf(AlarmKind::Low));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 70)), kindOf(AlarmKind::None));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 279)), kindOf(AlarmKind::None));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 280)), kindOf(AlarmKind::High));
    // No 401 cap: a reading too high for the clocks is not one to be quiet about.
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(settings, 1000)), kindOf(AlarmKind::High));
}

STIPPLE_TEST(GlucoseAlarm, OnlyUrgentLowIsOnByDefault) {
    const GlucoseAlarmSettings defaults;
    STIPPLE_CHECK(defaults.urgentLow.enabled);
    STIPPLE_CHECK_FALSE(defaults.low.enabled);
    STIPPLE_CHECK_FALSE(defaults.high.enabled);
    STIPPLE_CHECK_FALSE(defaults.noData.enabled);
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(defaults, 50)), kindOf(AlarmKind::UrgentLow));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(defaults, 65)), kindOf(AlarmKind::None));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(defaults, 300)), kindOf(AlarmKind::None));

    // Low disabled: its range still starts above urgent low, so with urgent low
    // also disabled nothing sounds at 50 - the clocks' rule, kept.
    GlucoseAlarmSettings lowOnly;
    lowOnly.urgentLow.enabled = false;
    lowOnly.low.enabled = true;
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(lowOnly, 50)), kindOf(AlarmKind::None));
    STIPPLE_CHECK_EQ(kindOf(classifyOnce(lowOnly, 60)), kindOf(AlarmKind::Low));
}

STIPPLE_TEST(GlucoseAlarm, ADefaultReadingNeverSounds) {
    // Before the first fetch the source's reading is sgv 0, minutesAgo 0: not
    // stale, and nothing like a real 0. It must not be taken for a low.
    GlucoseAlarm alarm;
    AlarmInputs in;
    in.nowMillis = 1000;
    in.configured = true;
    in.fresh = false;
    in.sgv = 0;
    STIPPLE_CHECK_FALSE(alarm.tick(in).play);
    in.fresh = true;  // even if a caller got freshness wrong
    STIPPLE_CHECK_FALSE(alarm.tick(in).play);
}

STIPPLE_TEST(GlucoseAlarm, SoundsAtOnceThenEveryRepeatInterval) {
    GlucoseAlarm alarm;
    std::uint64_t now = 10 * kMinute;
    auto first = alarm.tick(reading(50, now));
    STIPPLE_CHECK(first.play);
    STIPPLE_CHECK(first.fresh);
    STIPPLE_CHECK(alarm.sounding());

    now += 299000;
    STIPPLE_CHECK_FALSE(alarm.tick(reading(50, now)).play);
    now += 1000;  // 300 s, the default repeat
    const auto repeat = alarm.tick(reading(50, now));
    STIPPLE_CHECK(repeat.play);
    STIPPLE_CHECK_FALSE(repeat.fresh);

    GlucoseAlarmSettings fast;
    fast.repeatSeconds = 60;
    alarm.configure(fast);
    now += 60000;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);
}

STIPPLE_TEST(GlucoseAlarm, IntensiveRepeatsTwoSecondsAfterTheMelody) {
    GlucoseAlarmSettings settings;
    settings.intensive = true;
    GlucoseAlarm alarm;
    alarm.configure(settings);
    const int melody = alarm.melodyFor(AlarmKind::UrgentLow).totalMillis();
    STIPPLE_CHECK(melody > 0);

    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);
    STIPPLE_CHECK_FALSE(
        alarm.tick(reading(50, now + static_cast<std::uint64_t>(melody) + 1999)).play);
    STIPPLE_CHECK(alarm.tick(reading(50, now + static_cast<std::uint64_t>(melody) + 2000)).play);
}

STIPPLE_TEST(GlucoseAlarm, SnoozeCountsFromThePressAndThenSoundsAgain) {
    GlucoseAlarm alarm;  // urgent low snoozes for 15 minutes by default
    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);

    now += 2 * kMinute;
    STIPPLE_CHECK(alarm.snooze(now));
    STIPPLE_CHECK(alarm.snoozed());
    STIPPLE_CHECK_FALSE(alarm.sounding());
    STIPPLE_CHECK_FALSE(alarm.snooze(now));  // already snoozed: the press is not used
    STIPPLE_CHECK_EQ(static_cast<int>(alarm.snoozeRemainingSeconds(now)), 900);

    // Past the 300 s repeat, still quiet.
    STIPPLE_CHECK_FALSE(alarm.tick(reading(50, now + 10 * kMinute)).play);
    STIPPLE_CHECK_FALSE(alarm.tick(reading(50, now + 15 * kMinute - 1)).play);
    // Fifteen minutes from the press, not from the last sound.
    const auto back = alarm.tick(reading(50, now + 15 * kMinute));
    STIPPLE_CHECK(back.play);
    STIPPLE_CHECK(alarm.sounding());
}

STIPPLE_TEST(GlucoseAlarm, SnoozeZeroLastsUntilTheReadingLeaves) {
    GlucoseAlarmSettings settings;
    settings.urgentLow.snoozeMinutes = 0;
    GlucoseAlarm alarm;
    alarm.configure(settings);
    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);
    STIPPLE_CHECK(alarm.snooze(now));
    STIPPLE_CHECK_EQ(static_cast<int>(alarm.snoozeRemainingSeconds(now)), -1);
    STIPPLE_CHECK_FALSE(alarm.tick(reading(50, now + 600 * kMinute)).play);

    // Out of range re-arms; back in sounds at once.
    STIPPLE_CHECK_FALSE(alarm.tick(reading(100, now + 601 * kMinute)).play);
    STIPPLE_CHECK_EQ(kindOf(alarm.active()), kindOf(AlarmKind::None));
    STIPPLE_CHECK(alarm.tick(reading(50, now + 602 * kMinute)).play);
}

STIPPLE_TEST(GlucoseAlarm, ALesserSnoozeNeverSilencesAWorseAlarm) {
    GlucoseAlarm alarm;
    alarm.configure(allOn());
    std::uint64_t now = kMinute;
    STIPPLE_CHECK_EQ(kindOf(alarm.tick(reading(65, now)).kind), kindOf(AlarmKind::Low));
    STIPPLE_CHECK(alarm.snooze(now));

    // The clocks would stay quiet here for the rest of the low's snooze.
    now += kMinute;
    const auto worse = alarm.tick(reading(52, now));
    STIPPLE_CHECK_EQ(kindOf(worse.kind), kindOf(AlarmKind::UrgentLow));
    STIPPLE_CHECK(worse.play);
    STIPPLE_CHECK(worse.fresh);
    STIPPLE_CHECK(alarm.sounding());
}

STIPPLE_TEST(GlucoseAlarm, AWorseSnoozeCoversTheRecoveryIntoLow) {
    GlucoseAlarm alarm;
    alarm.configure(allOn());
    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);
    STIPPLE_CHECK(alarm.snooze(now));

    now += kMinute;
    const auto better = alarm.tick(reading(62, now));
    STIPPLE_CHECK_EQ(kindOf(better.kind), kindOf(AlarmKind::Low));
    STIPPLE_CHECK_FALSE(better.play);
    STIPPLE_CHECK(alarm.snoozed());

    // And back down to urgent low under the same snooze: still covered.
    STIPPLE_CHECK_FALSE(alarm.tick(reading(54, now + kMinute)).play);
}

STIPPLE_TEST(GlucoseAlarm, ChangingSideIsANewAlarm) {
    GlucoseAlarm alarm;
    alarm.configure(allOn());
    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(300, now)).play);
    STIPPLE_CHECK(alarm.snooze(now));
    // A snoozed high says nothing about a low.
    STIPPLE_CHECK(alarm.tick(reading(60, now + kMinute)).play);
}

STIPPLE_TEST(GlucoseAlarm, StaleDataSilencesAndResets) {
    GlucoseAlarm alarm;
    std::uint64_t now = kMinute;
    STIPPLE_CHECK(alarm.tick(reading(50, now)).play);
    STIPPLE_CHECK(alarm.snooze(now));

    AlarmInputs stale = reading(50, now + kMinute);
    stale.fresh = false;
    stale.dataAgeMinutes = 21;
    STIPPLE_CHECK_FALSE(alarm.tick(stale).play);
    STIPPLE_CHECK_EQ(kindOf(alarm.active()), kindOf(AlarmKind::None));

    // Fresh again and still low: a new alarm, the old snooze forgotten.
    STIPPLE_CHECK(alarm.tick(reading(50, now + 2 * kMinute)).play);
}

STIPPLE_TEST(GlucoseAlarm, NoDataSoundsWhenTheSourceGoesQuiet) {
    GlucoseAlarmSettings settings;
    settings.noData.enabled = true;
    settings.noDataMinutes = 30;
    GlucoseAlarm alarm;
    alarm.configure(settings);

    AlarmInputs in;
    in.nowMillis = kMinute;
    in.configured = true;
    in.fresh = false;
    in.dataAgeMinutes = 29;
    STIPPLE_CHECK_FALSE(alarm.tick(in).play);
    in.dataAgeMinutes = 30;
    in.nowMillis += kMinute;
    const auto quiet = alarm.tick(in);
    STIPPLE_CHECK(quiet.play);
    STIPPLE_CHECK_EQ(kindOf(quiet.kind), kindOf(AlarmKind::NoData));

    // Unknown age - no wall clock, or never configured - is never no-data.
    GlucoseAlarm other;
    other.configure(settings);
    in.dataAgeMinutes = -1;
    STIPPLE_CHECK_FALSE(other.tick(in).play);
    in.dataAgeMinutes = 90;
    in.configured = false;
    STIPPLE_CHECK_FALSE(other.tick(in).play);
}

STIPPLE_TEST(GlucoseAlarm, DisabledMeansSilent) {
    GlucoseAlarmSettings settings;
    settings.urgentLow.enabled = false;
    GlucoseAlarm alarm;
    alarm.configure(settings);
    STIPPLE_CHECK_FALSE(alarm.tick(reading(40, kMinute)).play);

    // Disabling an alarm that is sounding ends it on the next tick.
    GlucoseAlarm sounding;
    STIPPLE_CHECK(sounding.tick(reading(40, kMinute)).play);
    sounding.configure(settings);
    sounding.tick(reading(40, 2 * kMinute));
    STIPPLE_CHECK_EQ(kindOf(sounding.active()), kindOf(AlarmKind::None));
}

STIPPLE_TEST(GlucoseAlarm, LocalDayAndMinute) {
    int weekday = -1;
    int minutes = -1;
    localDayAndMinute(0, 0, weekday, minutes);  // 1970-01-01 00:00, a Thursday
    STIPPLE_CHECK_EQ(weekday, 4);
    STIPPLE_CHECK_EQ(minutes, 0);
    // 2026-09-30 22:30 EDT = 2026-10-01 02:30 UTC, a Wednesday locally.
    localDayAndMinute(1790821800, -4 * 3600, weekday, minutes);
    STIPPLE_CHECK_EQ(weekday, 3);
    STIPPLE_CHECK_EQ(minutes, 22 * 60 + 30);
    // West of UTC before the epoch still floors correctly.
    localDayAndMinute(0, -3600, weekday, minutes);
    STIPPLE_CHECK_EQ(weekday, 3);
    STIPPLE_CHECK_EQ(minutes, 23 * 60);
}

STIPPLE_TEST(GlucoseAlarm, AlertWindowsFollowTheClocks) {
    AlertWindow weekdayNights;
    weekdayNights.dayMask = 0x3E;  // Monday to Friday
    weekdayNights.fromMinutes = 22 * 60;
    weekdayNights.toMinutes = 7 * 60;

    STIPPLE_CHECK(insideWindow(weekdayNights, 1, 23 * 60));      // Monday 23:00
    STIPPLE_CHECK(insideWindow(weekdayNights, 2, 6 * 60));       // Tuesday 06:00, Monday's night
    STIPPLE_CHECK_FALSE(insideWindow(weekdayNights, 2, 7 * 60)); // ends at 07:00
    STIPPLE_CHECK(insideWindow(weekdayNights, 6, 3 * 60));       // Saturday 03:00 is Friday's
    STIPPLE_CHECK_FALSE(insideWindow(weekdayNights, 0, 3 * 60)); // Sunday 03:00 is Saturday's
    STIPPLE_CHECK_FALSE(insideWindow(weekdayNights, 6, 23 * 60));
    STIPPLE_CHECK(insideWindow(weekdayNights, 1, 6 * 60) == false);  // Sunday night not chosen

    AlertWindow daytime;
    daytime.dayMask = 0x7F;
    daytime.fromMinutes = 8 * 60;
    daytime.toMinutes = 20 * 60;
    STIPPLE_CHECK(insideWindow(daytime, 3, 8 * 60));
    STIPPLE_CHECK_FALSE(insideWindow(daytime, 3, 20 * 60));
}

STIPPLE_TEST(GlucoseAlarm, OutsideItsWindowAnAlarmIsNotInForce) {
    GlucoseAlarmSettings settings;
    settings.urgentLow.windows[0].dayMask = 0x7F;
    settings.urgentLow.windows[0].fromMinutes = 8 * 60;
    settings.urgentLow.windows[0].toMinutes = 20 * 60;
    settings.urgentLow.windowCount = 1;
    GlucoseAlarm alarm;
    alarm.configure(settings);

    AlarmInputs in = reading(50, kMinute);
    in.clockValid = true;
    in.weekday = 3;
    in.localMinutes = 21 * 60;
    STIPPLE_CHECK_FALSE(alarm.tick(in).play);
    STIPPLE_CHECK_EQ(kindOf(alarm.active()), kindOf(AlarmKind::None));

    // No wall clock: windows are ignored and it sounds - the safe direction.
    in.clockValid = false;
    STIPPLE_CHECK(alarm.tick(in).play);
}

STIPPLE_TEST(GlucoseAlarm, ABadStoredMelodyFallsBackToTheDefault) {
    GlucoseAlarmSettings settings;
    settings.urgentLow.melody = "this is not rtttl";
    GlucoseAlarm alarm;
    const std::uint32_t before = alarm.melodyFallbacks();
    alarm.configure(settings);
    STIPPLE_CHECK_EQ(static_cast<int>(alarm.melodyFallbacks()), static_cast<int>(before) + 1);
    STIPPLE_CHECK(alarm.melodyFor(AlarmKind::UrgentLow).audible());
}

STIPPLE_TEST(GlucoseAlarmSettings, PatchIsStrict) {
    std::string error;
    GlucoseAlarmSettings settings;
    STIPPLE_CHECK(apply(R"({"low":{"enabled":true,"mgdl":75,"snoozeMinutes":10}})", settings,
                        error));
    STIPPLE_CHECK(settings.low.enabled);
    STIPPLE_CHECK_EQ(settings.low.mgdl, 75);
    STIPPLE_CHECK_EQ(settings.low.snoozeMinutes, 10);

    const char* const refused[] = {
        R"({"low":{"enabled":"yes"}})",
        R"({"low":{"mgdl":"70"}})",
        R"({"low":{"mgdl":70.5}})",
        R"({"low":{"mgdl":29}})",
        R"({"low":{"mgdl":400}})",
        R"({"low":{"snoozeMinutes":7}})",
        R"({"low":{"melody":"nope"}})",
        R"({"low":{"melody":"quiet:d=4,o=5,b=100:p"}})",
        R"({"low":{"windows":{}}})",
        R"({"low":{"windows":[{"days":"7","from":"22:00","to":"07:00"}]}})",
        R"({"low":{"windows":[{"days":"11","from":"22:00","to":"07:00"}]}})",
        R"({"low":{"windows":[{"days":"1","from":"24:00","to":"07:00"}]}})",
        R"({"low":{"windows":[{"days":"1","from":"07:00","to":"07:00"}]}})",
        R"({"low":{"mgdl":50}})",          // below urgent low's 55
        R"({"high":{"mgdl":60}})",         // below low's 70
        R"({"noData":{"minutes":25}})",
        R"({"repeatSeconds":90})",
        R"({"intensive":1})",
        R"({"volumePercent":0})",
        R"({"volumePercent":101})",
        R"({"urgentLow":true})",
    };
    for (const char* text : refused) {
        GlucoseAlarmSettings copy;
        error.clear();
        STIPPLE_CHECK_FALSE(apply(text, copy, error));
        STIPPLE_CHECK_FALSE(error.empty());
    }
}

STIPPLE_TEST(GlucoseAlarmSettings, WindowsReplaceTheListAndRoundTrip) {
    std::string error;
    GlucoseAlarmSettings settings;
    STIPPLE_REQUIRE(apply(
        R"({"urgentLow":{"windows":[{"days":"54321","from":"22:00","to":"07:30"},{"days":"06","from":"00:00","to":"23:59"}]},"noData":{"enabled":true,"minutes":45},"repeatSeconds":120,"intensive":true,"volumePercent":80})",
        settings, error));
    STIPPLE_CHECK_EQ(settings.urgentLow.windowCount, 2);
    STIPPLE_CHECK_EQ(static_cast<int>(settings.urgentLow.windows[0].dayMask), 0x3E);
    STIPPLE_CHECK_EQ(static_cast<int>(settings.urgentLow.windows[0].toMinutes), 7 * 60 + 30);
    STIPPLE_CHECK_EQ(settings.noDataMinutes, 45);

    const std::string json = stipple::config::alarmSettingsJson(settings);
    STIPPLE_CHECK(json.find(R"("days":"12345","from":"22:00","to":"07:30")") != std::string::npos);

    // What is written is exactly what is read.
    GlucoseAlarmSettings again;
    STIPPLE_REQUIRE(apply(json, again, error));
    STIPPLE_CHECK(again == settings);

    STIPPLE_REQUIRE(apply(R"({"urgentLow":{"windows":[]}})", settings, error));
    STIPPLE_CHECK_EQ(settings.urgentLow.windowCount, 0);
}

STIPPLE_TEST(GlucoseAlarmSettings, StoredAlarmsSurviveAndBadOnesFallBack) {
    using stipple::config::Config;
    using stipple::config::ConfigStore;
    using stipple::config::LoadStatus;
    stipple::platform::simulator::SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.glucose.alarms = allOn();
    written.glucose.alarms.high.mgdl = 250;
    written.glucose.alarms.volumePercent = 60;
    STIPPLE_REQUIRE(store.save(written));
    Config read;
    STIPPLE_CHECK(store.load(read).status == LoadStatus::Loaded);
    STIPPLE_CHECK(read.glucose.alarms == written.glucose.alarms);

    // A stored rule that no longer validates comes back as its default, and the
    // rest of the alarms - and the rest of the config - load untouched.
    GlucoseAlarmSettings loaded;
    stipple::json::Token tokens[512];
    stipple::json::Document document(tokens, 512);
    STIPPLE_REQUIRE(document.parse(
        R"({"urgentLow":{"enabled":true,"mgdl":55,"snoozeMinutes":15,"windows":[],"melody":"broken"},"high":{"enabled":true,"mgdl":250,"snoozeMinutes":60,"windows":[],"melody":"high:d=4,o=5,b=125:4e7,p,4e7"},"volumePercent":60})") ==
                    stipple::json::Error::None);
    const int fellBack = stipple::config::loadAlarmSettings(document.root(), loaded);
    STIPPLE_CHECK_EQ(fellBack, 1);
    STIPPLE_CHECK_EQ(loaded.urgentLow.melody, std::string(stipple::config::kUrgentLowMelody));
    STIPPLE_CHECK(loaded.urgentLow.enabled);
    STIPPLE_CHECK(loaded.high.enabled);
    STIPPLE_CHECK_EQ(loaded.high.mgdl, 250);
    STIPPLE_CHECK_EQ(loaded.volumePercent, 60);

    // A config from before alarms existed gets the defaults: urgent low on.
    Config old;
    STIPPLE_CHECK(old.glucose.alarms.urgentLow.enabled);
}
