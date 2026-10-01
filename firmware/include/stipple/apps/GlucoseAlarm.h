// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "stipple/audio/Melody.h"
#include "stipple/config/GlucoseAlarmSettings.h"

namespace stipple {
namespace apps {
namespace glucose {

enum class AlarmKind : std::uint8_t {
    None,
    UrgentLow,
    Low,
    High,
    NoData,
};

/// "none", "urgentLow", "low", "high", "noData" - the settings keys.
const char* alarmKindName(AlarmKind kind) noexcept;

/// Everything the alarm needs to know about now. Built by the host each tick.
struct AlarmInputs {
    std::uint64_t nowMillis = 0;

    /// A source URL is configured.
    bool configured = false;

    /// The reading is real and current: at least one sample, not stale. A
    /// default Reading has sgv 0 and minutesAgo 0 and would otherwise look
    /// fresh - which is exactly the reading a device has before its first
    /// fetch.
    bool fresh = false;
    int sgv = 0;

    /// Minutes since the newest sample, or since the source became able to
    /// fetch when there has never been one. -1 = unknown (no wall clock, or
    /// not configured), which never counts as no-data.
    int dataAgeMinutes = -1;

    /// Local time, for alert windows. Without a wall clock the windows are
    /// ignored and the alarm sounds - nightscout-clock's rule, and the safe
    /// direction.
    bool clockValid = false;
    int localMinutes = 0;  ///< 0-1439
    int weekday = 0;       ///< 0 = Sunday
};

/// Weekday (0 = Sunday) and local minute of the day from Unix time.
void localDayAndMinute(std::int64_t unixSeconds, int utcOffsetSeconds, int& weekday,
                       int& minutes) noexcept;

/// True if `window` covers this weekday and minute. A window that wraps past
/// midnight belongs to the day it starts on.
bool insideWindow(const config::AlertWindow& window, int weekday, int minutes) noexcept;

/// The glucose alarm, as a pure state machine (Stage 4).
///
/// Modelled on nightscout-clock's BGAlarmManager so the owner's clocks and
/// this panel behave alike, with two deliberate differences:
///
/// - **A lesser snooze never silences a worse alarm.** On the clocks a low
///   that is snoozed and then drops into urgent low inherits the low alarm's
///   snooze. Here urgent low sounds at once. The other way round - urgent low
///   snoozed, recovering into low - stays quiet until the snooze ends.
/// - **Snooze counts from the press**, not from the last time it sounded.
///
/// It never touches the speaker or the screen: tick() says whether to sound
/// now, and the host acts. That keeps every rule here testable without one.
class GlucoseAlarm {
public:
    GlucoseAlarm();

    /// Take new settings. Cheap when nothing changed, so it is called every
    /// tick; melodies are parsed only on a change. A melody that will not parse
    /// - which the settings layer should have made impossible - falls back to
    /// that alarm's default, never to silence, and is counted.
    void configure(const config::GlucoseAlarmSettings& settings);

    struct Decision {
        /// The alarm in force, sounding or snoozed. None when nothing is.
        AlarmKind kind = AlarmKind::None;
        /// Play the melody now.
        bool play = false;
        /// This play is a new alarm or an escalation, not a repeat.
        bool fresh = false;
    };

    Decision tick(const AlarmInputs& inputs);

    /// The knob press. Snoozes the alarm in force for its snooze length and
    /// returns true; returns false when nothing was sounding (no alarm, or
    /// already snoozed), so the press can do whatever it normally does.
    bool snooze(std::uint64_t nowMillis);

    /// The host played (or failed to play) the melody tick() asked for.
    void notePlayed(bool played, std::uint64_t nowMillis) noexcept;

    AlarmKind active() const noexcept { return active_; }

    /// An alarm is in force and nobody has snoozed it: the state in which it
    /// takes the screen and a knob press means "snooze".
    bool sounding() const noexcept { return active_ != AlarmKind::None && !snoozed_; }

    bool snoozed() const noexcept { return active_ != AlarmKind::None && snoozed_; }

    /// Whole seconds of snooze left; -1 for "until it leaves the range", 0
    /// when not snoozed.
    std::int64_t snoozeRemainingSeconds(std::uint64_t nowMillis) const noexcept;

    /// The snooze length the alarm in force would get, in minutes.
    int snoozeMinutes() const noexcept;

    const audio::Melody& melodyFor(AlarmKind kind) const noexcept;

    /// Until when an alarm melody (or a test of one) owns the speaker, so
    /// nothing else's beep cuts it off. The host feeds this to SharedSpeaker.
    void holdSpeaker(std::uint64_t untilMillis) noexcept { speakerHeldUntil_ = untilMillis; }
    std::uint64_t speakerHeldUntil() const noexcept { return speakerHeldUntil_; }
    int volumePercent() const noexcept { return settings_.volumePercent; }

    // Diagnostics.
    std::uint32_t plays() const noexcept { return plays_; }
    std::uint32_t playFailures() const noexcept { return playFailures_; }
    std::uint32_t melodyFallbacks() const noexcept { return melodyFallbacks_; }
    std::uint64_t lastPlayMillis() const noexcept { return lastPlayMillis_; }

private:
    /// Which side of the range an alarm is on, and how bad it is there.
    static int side(AlarmKind kind) noexcept;
    static int severity(AlarmKind kind) noexcept;

    const config::AlarmRule& rule(AlarmKind kind) const noexcept;

    /// What the inputs call for, before snooze and repeat.
    AlarmKind classify(const AlarmInputs& inputs) const noexcept;

    bool allowedNow(AlarmKind kind, const AlarmInputs& inputs) const noexcept;

    void reset() noexcept;

    config::GlucoseAlarmSettings settings_;
    bool configured_ = false;

    audio::Melody urgentLowMelody_;
    audio::Melody lowMelody_;
    audio::Melody highMelody_;
    audio::Melody noDataMelody_;

    AlarmKind active_ = AlarmKind::None;
    std::uint64_t lastSoundMillis_ = 0;
    bool soundedOnce_ = false;

    bool snoozed_ = false;
    int snoozeSide_ = 0;
    int snoozeSeverity_ = 0;
    bool snoozeUntilClear_ = false;
    std::uint64_t snoozeUntilMillis_ = 0;

    std::uint32_t plays_ = 0;
    std::uint32_t playFailures_ = 0;
    std::uint32_t melodyFallbacks_ = 0;
    std::uint64_t lastPlayMillis_ = 0;
    std::uint64_t speakerHeldUntil_ = 0;
};

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
