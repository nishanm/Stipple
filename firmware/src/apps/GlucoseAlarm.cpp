// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseAlarm.h"

namespace stipple {
namespace apps {
namespace glucose {
namespace {

constexpr std::int64_t kSecondsPerDay = 86400;

/// Intensive mode's gap between the end of one melody and the next.
constexpr std::uint64_t kIntensiveGapMillis = 2000;

/// Parse `text`, or the built-in `fallback` if it will not, counting the
/// fallback. Never leaves a silent melody behind.
void compile(const std::string& text, const char* fallback, audio::Melody& out,
             std::uint32_t& fallbacks) {
    if (audio::parseRtttl(text, out) == audio::RtttlError::None) {
        return;
    }
    ++fallbacks;
    audio::parseRtttl(fallback, out);
}

}  // namespace

const char* alarmKindName(AlarmKind kind) noexcept {
    switch (kind) {
        case AlarmKind::None: return "none";
        case AlarmKind::UrgentLow: return "urgentLow";
        case AlarmKind::Low: return "low";
        case AlarmKind::High: return "high";
        case AlarmKind::NoData: return "noData";
    }
    return "none";
}

void localDayAndMinute(std::int64_t unixSeconds, int utcOffsetSeconds, int& weekday,
                       int& minutes) noexcept {
    const std::int64_t local = unixSeconds + utcOffsetSeconds;
    std::int64_t days = local / kSecondsPerDay;
    std::int64_t seconds = local % kSecondsPerDay;
    if (seconds < 0) {
        seconds += kSecondsPerDay;
        --days;
    }
    minutes = static_cast<int>(seconds / 60);
    // 1970-01-01 was a Thursday.
    weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);
}

bool insideWindow(const config::AlertWindow& window, int weekday, int minutes) noexcept {
    const auto startsOn = [&window](int day) {
        return (window.dayMask & (1u << static_cast<unsigned>(day))) != 0;
    };
    if (window.fromMinutes < window.toMinutes) {
        return startsOn(weekday) && minutes >= window.fromMinutes && minutes < window.toMinutes;
    }
    // Past midnight: the evening part on a day it starts, the morning part on
    // the day after one.
    return (startsOn(weekday) && minutes >= window.fromMinutes) ||
           (startsOn((weekday + 6) % 7) && minutes < window.toMinutes);
}

GlucoseAlarm::GlucoseAlarm() { configure(config::GlucoseAlarmSettings{}); }

void GlucoseAlarm::configure(const config::GlucoseAlarmSettings& settings) {
    if (configured_ && settings == settings_) {
        return;
    }
    settings_ = settings;
    configured_ = true;
    compile(settings_.urgentLow.melody, config::kUrgentLowMelody, urgentLowMelody_,
            melodyFallbacks_);
    compile(settings_.low.melody, config::kLowMelody, lowMelody_, melodyFallbacks_);
    compile(settings_.high.melody, config::kHighMelody, highMelody_, melodyFallbacks_);
    compile(settings_.noData.melody, config::kNoDataMelody, noDataMelody_, melodyFallbacks_);
}

int GlucoseAlarm::side(AlarmKind kind) noexcept {
    switch (kind) {
        case AlarmKind::UrgentLow:
        case AlarmKind::Low: return 1;
        case AlarmKind::High: return 2;
        case AlarmKind::NoData: return 3;
        case AlarmKind::None: break;
    }
    return 0;
}

int GlucoseAlarm::severity(AlarmKind kind) noexcept {
    return kind == AlarmKind::UrgentLow ? 2 : (kind == AlarmKind::None ? 0 : 1);
}

const config::AlarmRule& GlucoseAlarm::rule(AlarmKind kind) const noexcept {
    switch (kind) {
        case AlarmKind::UrgentLow: return settings_.urgentLow;
        case AlarmKind::Low: return settings_.low;
        case AlarmKind::High: return settings_.high;
        case AlarmKind::NoData:
        case AlarmKind::None: break;
    }
    return settings_.noData;
}

const audio::Melody& GlucoseAlarm::melodyFor(AlarmKind kind) const noexcept {
    switch (kind) {
        case AlarmKind::UrgentLow: return urgentLowMelody_;
        case AlarmKind::Low: return lowMelody_;
        case AlarmKind::High: return highMelody_;
        case AlarmKind::NoData:
        case AlarmKind::None: break;
    }
    return noDataMelody_;
}

AlarmKind GlucoseAlarm::classify(const AlarmInputs& inputs) const noexcept {
    if (inputs.fresh && inputs.sgv > 0) {
        const int sgv = inputs.sgv;
        const int urgent = settings_.urgentLow.mgdl;
        // nightscout-clock's ranges: urgent low [1, U]; low [U+1, L-1] whether
        // or not urgent low is enabled; high from H up. The clocks cap high at
        // 401 because their parser does; ours accepts up to 1000, and a reading
        // too high to cap is not one to be quiet about.
        if (settings_.urgentLow.enabled && sgv <= urgent) {
            return AlarmKind::UrgentLow;
        }
        if (settings_.low.enabled && sgv > urgent && sgv < settings_.low.mgdl) {
            return AlarmKind::Low;
        }
        if (settings_.high.enabled && sgv >= settings_.high.mgdl) {
            return AlarmKind::High;
        }
        return AlarmKind::None;
    }

    // Not fresh: stale glucose silences the glucose alarms, as on the clocks.
    // The no-data alarm is the net under that - off by default, the owner's
    // choice - and it measures from the newest sample, or from when fetching
    // became possible if there has never been one.
    if (settings_.noData.enabled && inputs.configured && inputs.dataAgeMinutes >= 0 &&
        inputs.dataAgeMinutes >= settings_.noDataMinutes) {
        return AlarmKind::NoData;
    }
    return AlarmKind::None;
}

bool GlucoseAlarm::allowedNow(AlarmKind kind, const AlarmInputs& inputs) const noexcept {
    const config::AlarmRule& which = rule(kind);
    if (which.windowCount == 0 || !inputs.clockValid) {
        return true;
    }
    for (int i = 0; i < which.windowCount; ++i) {
        if (insideWindow(which.windows[i], inputs.weekday, inputs.localMinutes)) {
            return true;
        }
    }
    return false;
}

void GlucoseAlarm::reset() noexcept {
    active_ = AlarmKind::None;
    lastSoundMillis_ = 0;
    soundedOnce_ = false;
    snoozed_ = false;
    snoozeSide_ = 0;
    snoozeSeverity_ = 0;
    snoozeUntilClear_ = false;
    snoozeUntilMillis_ = 0;
}

GlucoseAlarm::Decision GlucoseAlarm::tick(const AlarmInputs& inputs) {
    Decision decision;
    const std::uint64_t now = inputs.nowMillis;

    AlarmKind kind = classify(inputs);
    // Outside its alert windows an alarm is not merely quiet, it is not in
    // force: leaving a window re-arms it, as on the clocks.
    if (kind != AlarmKind::None && !allowedNow(kind, inputs)) {
        kind = AlarmKind::None;
    }
    if (kind == AlarmKind::None) {
        reset();
        return decision;
    }

    bool snoozeEnded = false;
    if (snoozed_ && !snoozeUntilClear_ && now >= snoozeUntilMillis_) {
        snoozed_ = false;
        snoozeEnded = true;
    }

    const bool covered = snoozed_ && snoozeSide_ == side(kind) &&
                         snoozeSeverity_ >= severity(kind);

    const bool newAlarm = active_ == AlarmKind::None || side(kind) != side(active_) ||
                          severity(kind) > severity(active_);
    active_ = kind;
    decision.kind = kind;

    if (covered) {
        return decision;
    }
    // A snooze that does not cover this alarm - another side, or a lesser
    // severity than what is now happening - is over.
    snoozed_ = false;

    bool play = newAlarm || snoozeEnded || !soundedOnce_;
    if (!play) {
        const std::uint64_t interval =
            settings_.intensive
                ? static_cast<std::uint64_t>(melodyFor(kind).totalMillis()) + kIntensiveGapMillis
                : static_cast<std::uint64_t>(settings_.repeatSeconds) * 1000u;
        play = now - lastSoundMillis_ >= interval;
    }
    if (play) {
        decision.play = true;
        decision.fresh = newAlarm;
        lastSoundMillis_ = now;
        soundedOnce_ = true;
    }
    return decision;
}

bool GlucoseAlarm::snooze(std::uint64_t nowMillis) {
    if (!sounding()) {
        return false;
    }
    const int minutes = rule(active_).snoozeMinutes;
    snoozed_ = true;
    snoozeSide_ = side(active_);
    snoozeSeverity_ = severity(active_);
    snoozeUntilClear_ = minutes == 0;
    snoozeUntilMillis_ = nowMillis + static_cast<std::uint64_t>(minutes) * 60000u;
    return true;
}

void GlucoseAlarm::notePlayed(bool played, std::uint64_t nowMillis) noexcept {
    if (played) {
        ++plays_;
        lastPlayMillis_ = nowMillis == 0 ? 1 : nowMillis;
    } else {
        ++playFailures_;
    }
}

std::int64_t GlucoseAlarm::snoozeRemainingSeconds(std::uint64_t nowMillis) const noexcept {
    if (!snoozed()) {
        return 0;
    }
    if (snoozeUntilClear_) {
        return -1;
    }
    if (nowMillis >= snoozeUntilMillis_) {
        return 0;
    }
    return static_cast<std::int64_t>((snoozeUntilMillis_ - nowMillis + 999) / 1000);
}

int GlucoseAlarm::snoozeMinutes() const noexcept {
    return active_ == AlarmKind::None ? 0 : rule(active_).snoozeMinutes;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
