// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace stipple {
namespace json {
class Value;
}
namespace config {

/// When an alarm may sound: on these days, between these times.
///
/// nightscout-clock's "alert windows", kept to the letter so the owner's
/// clocks and this panel mean the same thing by the same settings. An alarm
/// with no windows sounds at any time; with windows, only inside one.
struct AlertWindow {
    /// Bit n set = the window starts on weekday n (0 = Sunday, tm_wday).
    std::uint8_t dayMask = 0;
    /// Local minutes after midnight. `to` earlier than `from` runs past
    /// midnight, and the days are the days it starts on. Never equal.
    std::int16_t fromMinutes = 0;
    std::int16_t toMinutes = 0;

    bool operator==(const AlertWindow& other) const noexcept {
        return dayMask == other.dayMask && fromMinutes == other.fromMinutes &&
               toMinutes == other.toMinutes;
    }
};

/// One alarm's settings.
struct AlarmRule {
    static constexpr int kMaxWindows = 8;

    bool enabled = false;

    /// The threshold, mg/dL. Unused by the no-data alarm.
    int mgdl = 0;

    /// 0 = snoozed until the reading leaves the range; otherwise one of
    /// 5, 10, 15, 30, 60, 120.
    int snoozeMinutes = 15;

    AlertWindow windows[kMaxWindows] = {};
    int windowCount = 0;

    /// RTTTL text, validated whenever it is written.
    std::string melody;

    bool operator==(const AlarmRule& other) const noexcept;
    bool operator!=(const AlarmRule& other) const noexcept { return !(*this == other); }
};

/// nightscout-clock's defaults, verbatim, so a melody the owner already knows
/// by ear means the same thing here.
inline constexpr const char* kUrgentLowMelody =
    "urgent_low:d=4,o=5,b=230:4e6,4p,4e6,4p,4e6,4p,4e6";
inline constexpr const char* kLowMelody = "low:d=4,o=5,b=200:4e5,4p,4e5,4p,4e5";
inline constexpr const char* kHighMelody = "high:d=4,o=5,b=125:4e7,p,4e7";
/// The no-data alarm is ours; it borrows the clocks' "double beep" preset so
/// it sounds unlike any glucose alarm.
inline constexpr const char* kNoDataMelody = "doublebeep:d=8,o=6,b=180:c,p,c";

AlarmRule defaultUrgentLow();
AlarmRule defaultLow();
AlarmRule defaultHigh();
AlarmRule defaultNoData();

/// Every setting of the glucose alarm (Stage 4 of the glucose app).
struct GlucoseAlarmSettings {
    /// On by default: an urgent low is the one alarm spec A1 requires, and it
    /// can only ever sound on a fresh reading, which needs a configured source.
    AlarmRule urgentLow = defaultUrgentLow();
    AlarmRule low = defaultLow();
    AlarmRule high = defaultHigh();
    AlarmRule noData = defaultNoData();

    /// How old the newest reading may get before the no-data alarm sounds:
    /// 20, 30, 45 or 60.
    int noDataMinutes = 30;

    /// Seconds between repeats of an alarm nobody has snoozed: 60, 120, 300.
    int repeatSeconds = 300;

    /// Repeat two seconds after the melody ends instead, until snoozed.
    bool intensive = false;

    /// Absolute loudness of every alarm, 20-100 %. Independent of the device
    /// volume on purpose; there is no zero - to silence an alarm, disable it.
    int volumePercent = 100;

    bool operator==(const GlucoseAlarmSettings& other) const noexcept;
    bool operator!=(const GlucoseAlarmSettings& other) const noexcept {
        return !(*this == other);
    }
};

/// Apply a PATCH-shaped `alarms` object onto `settings`, strictly.
///
/// Absent fields keep their value. A present field of the wrong type or out
/// of range is refused with a message naming it, and `settings` may then be
/// half-written - callers apply it to a copy. A `windows` array replaces the
/// whole list. Thresholds must stay in order: urgent low < low < high.
bool applyAlarmSettings(const json::Value& alarms, GlucoseAlarmSettings& settings,
                        std::string& error);

/// Read a stored `alarms` object, forgivingly: each alarm that fails the strict
/// rules is replaced by its default rather than failing the load, because one
/// bad field must not cost the user every other setting (§21). Returns how many
/// parts fell back to defaults.
int loadAlarmSettings(const json::Value& alarms, GlucoseAlarmSettings& settings);

/// The canonical JSON object, shared by the stored config and the API so the
/// two can never disagree about a field's name or form.
std::string alarmSettingsJson(const GlucoseAlarmSettings& settings);

}  // namespace config
}  // namespace stipple
