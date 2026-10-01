// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/config/GlucoseAlarmSettings.h"

#include "stipple/audio/Melody.h"
#include "stipple/json/Json.h"

namespace stipple {
namespace config {
namespace {

constexpr int kMinThreshold = 30;
constexpr int kMaxThreshold = 399;

bool validSnooze(std::int64_t minutes) noexcept {
    return minutes == 0 || minutes == 5 || minutes == 10 || minutes == 15 || minutes == 30 ||
           minutes == 60 || minutes == 120;
}

/// A whole number, or false. 5.5 minutes is a client bug, not a request to
/// round.
bool readWhole(const json::Value& value, std::int64_t& out) {
    if (!value.isNumber()) {
        return false;
    }
    const double number = value.toDouble(-1.0);
    const std::int64_t whole = static_cast<std::int64_t>(number);
    if (static_cast<double>(whole) != number) {
        return false;
    }
    out = whole;
    return true;
}

std::string path(const char* rule, const char* field) {
    std::string text = "'glucose.alarms.";
    text += rule;
    if (field != nullptr) {
        text += '.';
        text += field;
    }
    text += '\'';
    return text;
}

/// "HH:MM" to minutes after midnight.
bool readTime(const json::Value& value, std::int16_t& out) {
    if (!value.isString()) {
        return false;
    }
    const std::string_view text = value.raw();
    if (text.size() != 5 || text[2] != ':') {
        return false;
    }
    for (const int i : {0, 1, 3, 4}) {
        if (text[static_cast<std::size_t>(i)] < '0' || text[static_cast<std::size_t>(i)] > '9') {
            return false;
        }
    }
    const int hours = (text[0] - '0') * 10 + (text[1] - '0');
    const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    if (hours > 23 || minutes > 59) {
        return false;
    }
    out = static_cast<std::int16_t>(hours * 60 + minutes);
    return true;
}

/// "0123456" (tm_wday digits, any order, each once) to a bit mask.
bool readDays(const json::Value& value, std::uint8_t& out) {
    if (!value.isString()) {
        return false;
    }
    const std::string_view text = value.raw();
    if (text.empty() || text.size() > 7) {
        return false;
    }
    std::uint8_t mask = 0;
    for (const char c : text) {
        if (c < '0' || c > '6') {
            return false;
        }
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (c - '0'));
        if ((mask & bit) != 0) {
            return false;
        }
        mask = static_cast<std::uint8_t>(mask | bit);
    }
    out = mask;
    return true;
}

bool applyRule(const json::Value& object, AlarmRule& rule, const char* name, bool hasThreshold,
               std::string& error) {
    if (!object.isObject()) {
        error = path(name, nullptr) + " must be an object";
        return false;
    }

    if (const json::Value value = object["enabled"]; value.valid()) {
        if (!value.isBoolean()) {
            error = path(name, "enabled") + " must be true or false";
            return false;
        }
        rule.enabled = value.toBool(false);
    }

    if (hasThreshold) {
        if (const json::Value value = object["mgdl"]; value.valid()) {
            std::int64_t mgdl = 0;
            if (!readWhole(value, mgdl) || mgdl < kMinThreshold || mgdl > kMaxThreshold) {
                error = path(name, "mgdl") + " must be a whole number, 30-399";
                return false;
            }
            rule.mgdl = static_cast<int>(mgdl);
        }
    }

    if (const json::Value value = object["snoozeMinutes"]; value.valid()) {
        std::int64_t minutes = 0;
        if (!readWhole(value, minutes) || !validSnooze(minutes)) {
            error = path(name, "snoozeMinutes") + " must be 0, 5, 10, 15, 30, 60 or 120";
            return false;
        }
        rule.snoozeMinutes = static_cast<int>(minutes);
    }

    if (const json::Value value = object["windows"]; value.valid()) {
        if (!value.isArray()) {
            error = path(name, "windows") + " must be an array";
            return false;
        }
        const int count = value.size();
        if (count > AlarmRule::kMaxWindows) {
            error = path(name, "windows") + " holds at most 8 windows";
            return false;
        }
        AlertWindow windows[AlarmRule::kMaxWindows] = {};
        for (int i = 0; i < count; ++i) {
            const json::Value entry = value[i];
            AlertWindow& window = windows[i];
            if (!entry.isObject() || !readDays(entry["days"], window.dayMask) ||
                !readTime(entry["from"], window.fromMinutes) ||
                !readTime(entry["to"], window.toMinutes)) {
                error = path(name, "windows") +
                        " entries need days (digits 0-6, 0 = Sunday) and from/to as HH:MM";
                return false;
            }
            if (window.fromMinutes == window.toMinutes) {
                error = path(name, "windows") + " entries need different from and to times";
                return false;
            }
        }
        for (int i = 0; i < AlarmRule::kMaxWindows; ++i) {
            rule.windows[i] = windows[i];
        }
        rule.windowCount = count;
    }

    if (const json::Value value = object["melody"]; value.valid()) {
        if (!value.isString()) {
            error = path(name, "melody") + " must be an RTTTL string";
            return false;
        }
        const std::string text = value.toString();
        audio::Melody melody;
        const audio::RtttlError parsed = audio::parseRtttl(text, melody);
        if (parsed != audio::RtttlError::None) {
            error = path(name, "melody") + ": " + audio::describe(parsed);
            return false;
        }
        rule.melody = text;
    }
    return true;
}

bool applyGlobals(const json::Value& alarms, GlucoseAlarmSettings& settings, std::string& error) {
    if (const json::Value value = alarms["repeatSeconds"]; value.valid()) {
        std::int64_t seconds = 0;
        if (!readWhole(value, seconds) || (seconds != 60 && seconds != 120 && seconds != 300)) {
            error = "'glucose.alarms.repeatSeconds' must be 60, 120 or 300";
            return false;
        }
        settings.repeatSeconds = static_cast<int>(seconds);
    }
    if (const json::Value value = alarms["intensive"]; value.valid()) {
        if (!value.isBoolean()) {
            error = "'glucose.alarms.intensive' must be true or false";
            return false;
        }
        settings.intensive = value.toBool(false);
    }
    if (const json::Value value = alarms["volumePercent"]; value.valid()) {
        std::int64_t percent = 0;
        if (!readWhole(value, percent) || percent < 20 || percent > 100) {
            error =
                "'glucose.alarms.volumePercent' must be 20-100 (disable an alarm to silence it)";
            return false;
        }
        settings.volumePercent = static_cast<int>(percent);
    }
    return true;
}

bool applyNoDataMinutes(const json::Value& noData, GlucoseAlarmSettings& settings,
                        std::string& error) {
    if (const json::Value value = noData["minutes"]; value.valid()) {
        std::int64_t minutes = 0;
        if (!readWhole(value, minutes) ||
            (minutes != 20 && minutes != 30 && minutes != 45 && minutes != 60)) {
            error = "'glucose.alarms.noData.minutes' must be 20, 30, 45 or 60";
            return false;
        }
        settings.noDataMinutes = static_cast<int>(minutes);
    }
    return true;
}

bool thresholdsInOrder(const GlucoseAlarmSettings& settings) noexcept {
    return settings.urgentLow.mgdl < settings.low.mgdl && settings.low.mgdl < settings.high.mgdl;
}

void appendString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void appendTime(std::string& out, int minutes) {
    const int hours = minutes / 60;
    const int rest = minutes % 60;
    out += '"';
    out += static_cast<char>('0' + hours / 10);
    out += static_cast<char>('0' + hours % 10);
    out += ':';
    out += static_cast<char>('0' + rest / 10);
    out += static_cast<char>('0' + rest % 10);
    out += '"';
}

void appendRule(std::string& out, const AlarmRule& rule, bool hasThreshold, int noDataMinutes) {
    out += "{\"enabled\":";
    out += rule.enabled ? "true" : "false";
    if (hasThreshold) {
        out += ",\"mgdl\":";
        out += std::to_string(rule.mgdl);
    } else {
        out += ",\"minutes\":";
        out += std::to_string(noDataMinutes);
    }
    out += ",\"snoozeMinutes\":";
    out += std::to_string(rule.snoozeMinutes);
    out += ",\"windows\":[";
    for (int i = 0; i < rule.windowCount; ++i) {
        const AlertWindow& window = rule.windows[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"days\":\"";
        for (int day = 0; day < 7; ++day) {
            if ((window.dayMask & (1u << day)) != 0) {
                out += static_cast<char>('0' + day);
            }
        }
        out += "\",\"from\":";
        appendTime(out, window.fromMinutes);
        out += ",\"to\":";
        appendTime(out, window.toMinutes);
        out += '}';
    }
    out += "],\"melody\":";
    appendString(out, rule.melody);
    out += '}';
}

AlarmRule makeRule(bool enabled, int mgdl, int snoozeMinutes, const char* melody) {
    AlarmRule rule;
    rule.enabled = enabled;
    rule.mgdl = mgdl;
    rule.snoozeMinutes = snoozeMinutes;
    rule.melody = melody;
    return rule;
}

}  // namespace

bool AlarmRule::operator==(const AlarmRule& other) const noexcept {
    if (enabled != other.enabled || mgdl != other.mgdl || snoozeMinutes != other.snoozeMinutes ||
        windowCount != other.windowCount || melody != other.melody) {
        return false;
    }
    for (int i = 0; i < windowCount; ++i) {
        if (!(windows[i] == other.windows[i])) {
            return false;
        }
    }
    return true;
}

bool GlucoseAlarmSettings::operator==(const GlucoseAlarmSettings& other) const noexcept {
    return urgentLow == other.urgentLow && low == other.low && high == other.high &&
           noData == other.noData && noDataMinutes == other.noDataMinutes &&
           repeatSeconds == other.repeatSeconds && intensive == other.intensive &&
           volumePercent == other.volumePercent;
}

AlarmRule defaultUrgentLow() { return makeRule(true, 55, 15, kUrgentLowMelody); }
AlarmRule defaultLow() { return makeRule(false, 70, 30, kLowMelody); }
AlarmRule defaultHigh() { return makeRule(false, 280, 60, kHighMelody); }
AlarmRule defaultNoData() { return makeRule(false, 0, 30, kNoDataMelody); }

bool applyAlarmSettings(const json::Value& alarms, GlucoseAlarmSettings& settings,
                        std::string& error) {
    if (!alarms.isObject()) {
        error = "'glucose.alarms' must be an object";
        return false;
    }
    struct Part {
        const char* key;
        AlarmRule* rule;
        bool hasThreshold;
    };
    const Part parts[] = {
        {"urgentLow", &settings.urgentLow, true},
        {"low", &settings.low, true},
        {"high", &settings.high, true},
        {"noData", &settings.noData, false},
    };
    for (const Part& part : parts) {
        const json::Value object = alarms[part.key];
        if (!object.valid()) {
            continue;
        }
        if (!applyRule(object, *part.rule, part.key, part.hasThreshold, error)) {
            return false;
        }
        if (!part.hasThreshold && !applyNoDataMinutes(object, settings, error)) {
            return false;
        }
    }
    if (!applyGlobals(alarms, settings, error)) {
        return false;
    }
    if (!thresholdsInOrder(settings)) {
        error = "'glucose.alarms' thresholds must rise: urgentLow.mgdl < low.mgdl < high.mgdl";
        return false;
    }
    return true;
}

int loadAlarmSettings(const json::Value& alarms, GlucoseAlarmSettings& settings) {
    if (!alarms.isObject()) {
        return 0;  // nothing stored yet: the defaults are the answer
    }
    int fellBack = 0;
    std::string ignored;

    struct Part {
        const char* key;
        AlarmRule* rule;
        AlarmRule (*fallback)();
        bool hasThreshold;
    };
    const Part parts[] = {
        {"urgentLow", &settings.urgentLow, &defaultUrgentLow, true},
        {"low", &settings.low, &defaultLow, true},
        {"high", &settings.high, &defaultHigh, true},
        {"noData", &settings.noData, &defaultNoData, false},
    };
    for (const Part& part : parts) {
        const json::Value object = alarms[part.key];
        if (!object.valid()) {
            continue;
        }
        AlarmRule rule = part.fallback();
        if (applyRule(object, rule, part.key, part.hasThreshold, ignored)) {
            *part.rule = rule;
        } else {
            *part.rule = part.fallback();
            ++fellBack;
        }
        if (!part.hasThreshold) {
            GlucoseAlarmSettings minutes;
            if (applyNoDataMinutes(object, minutes, ignored)) {
                settings.noDataMinutes = minutes.noDataMinutes;
            } else {
                ++fellBack;
            }
        }
    }

    GlucoseAlarmSettings globals;
    if (applyGlobals(alarms, globals, ignored)) {
        settings.repeatSeconds = globals.repeatSeconds;
        settings.intensive = globals.intensive;
        settings.volumePercent = globals.volumePercent;
    } else {
        ++fellBack;
    }

    if (!thresholdsInOrder(settings)) {
        settings.urgentLow.mgdl = defaultUrgentLow().mgdl;
        settings.low.mgdl = defaultLow().mgdl;
        settings.high.mgdl = defaultHigh().mgdl;
        ++fellBack;
    }
    return fellBack;
}

std::string alarmSettingsJson(const GlucoseAlarmSettings& settings) {
    std::string out;
    out.reserve(512);
    out += "{\"urgentLow\":";
    appendRule(out, settings.urgentLow, true, 0);
    out += ",\"low\":";
    appendRule(out, settings.low, true, 0);
    out += ",\"high\":";
    appendRule(out, settings.high, true, 0);
    out += ",\"noData\":";
    appendRule(out, settings.noData, false, settings.noDataMinutes);
    out += ",\"repeatSeconds\":";
    out += std::to_string(settings.repeatSeconds);
    out += ",\"intensive\":";
    out += settings.intensive ? "true" : "false";
    out += ",\"volumePercent\":";
    out += std::to_string(settings.volumePercent);
    out += '}';
    return out;
}

}  // namespace config
}  // namespace stipple
