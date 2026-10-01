// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/config/GlucoseFaceSettings.h"

#include <algorithm>

namespace stipple {
namespace config {
namespace {

using apps::GlucoseFace;
namespace plan = apps::glucose;

/// A selectable face name, exactly. "no-data" is a real face but not a choice.
bool selectableName(std::string_view name, GlucoseFace& out) noexcept {
    const GlucoseFace face = apps::glucoseFaceFromName(name);
    if (name != apps::glucoseFaceName(face) || face == GlucoseFace::NoData) {
        return false;
    }
    out = face;
    return true;
}

void appendTime(std::string& out, int minutes) {
    const int clamped = minutes < 0 ? 0 : (minutes > 1439 ? 1439 : minutes);
    const int hours = clamped / 60;
    const int mins = clamped % 60;
    out += '"';
    out += static_cast<char>('0' + hours / 10);
    out += static_cast<char>('0' + hours % 10);
    out += ':';
    out += static_cast<char>('0' + mins / 10);
    out += static_cast<char>('0' + mins % 10);
    out += '"';
}

/// "HH:MM" to minutes after midnight, 00:00-23:59.
bool readTime(const json::Value& value, int& out) {
    if (!value.isString()) {
        return false;
    }
    const std::string_view text = value.raw();
    if (text.size() != 5 || text[2] != ':') {
        return false;
    }
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    const int hours = (text[0] - '0') * 10 + (text[1] - '0');
    const int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    if (hours > 23 || minutes > 59) {
        return false;
    }
    out = hours * 60 + minutes;
    return true;
}

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

std::vector<std::string> namesOf(plan::FaceMask mask) {
    std::vector<std::string> names;
    for (int i = 0; i < apps::kGlucoseSelectableFaceCount; ++i) {
        if ((mask & (1u << static_cast<unsigned>(i))) != 0) {
            names.emplace_back(apps::glucoseFaceName(apps::glucoseFaceAt(i)));
        }
    }
    return names;
}

/// Insertion sort, stable: six rows at most, and no temporary buffer.
void sortRows(std::vector<GlucoseScheduleRow>& rows) {
    for (std::size_t i = 1; i < rows.size(); ++i) {
        for (std::size_t j = i; j > 0 && rows[j - 1].minutes > rows[j].minutes; --j) {
            std::swap(rows[j - 1], rows[j]);
        }
    }
}

/// Every cross-field rule, on settings whose individual fields already parsed.
bool checkWhole(const GlucoseSettings& s, std::string& error) {
    const plan::FaceMask mask = faceMaskOf(s);
    if (mask == 0) {
        error = "'glucose.faces' must name at least one face";
        return false;
    }
    GlucoseFace face = GlucoseFace::Hero;
    if (!selectableName(s.face, face) || !plan::faceActive(mask, face)) {
        error = "'glucose.face' must be one of 'glucose.faces'";
        return false;
    }
    if (!plan::cycleSecondsAllowed(s.cycleSeconds)) {
        error = "'glucose.cycleSeconds' must be 0, 10, 30, 60, 120, 180 or 300";
        return false;
    }
    if (s.cycleSeconds > 0 && plan::activeFaceCount(mask) < 2) {
        error = "'glucose.cycleSeconds' needs at least two faces in 'glucose.faces'";
        return false;
    }
    if (s.schedule.rows.size() > plan::kMaxScheduleRows) {
        error = "'glucose.schedule.rows' holds at most six rows";
        return false;
    }
    if (s.schedule.enabled && s.schedule.rows.empty()) {
        error = "'glucose.schedule' is enabled with no rows";
        return false;
    }
    for (std::size_t i = 0; i < s.schedule.rows.size(); ++i) {
        const GlucoseScheduleRow& row = s.schedule.rows[i];
        GlucoseFace rowFace = GlucoseFace::Hero;
        if (!selectableName(row.face, rowFace) || !plan::faceActive(mask, rowFace)) {
            error = "'glucose.schedule.rows' faces must be in 'glucose.faces'";
            return false;
        }
        if (i > 0 && s.schedule.rows[i - 1].minutes == row.minutes) {
            error = "'glucose.schedule.rows' has two rows at the same time";
            return false;
        }
    }
    return true;
}

}  // namespace

plan::FaceMask faceMaskOf(const GlucoseSettings& settings) noexcept {
    plan::FaceMask mask = 0;
    for (const std::string& name : settings.faces) {
        GlucoseFace face = GlucoseFace::Hero;
        if (selectableName(name, face)) {
            mask = static_cast<plan::FaceMask>(mask | plan::faceBit(face));
        }
    }
    return mask;
}

std::size_t scheduleRowsOf(const GlucoseSettings& settings, plan::ScheduleRow* out,
                           std::size_t capacity) noexcept {
    std::size_t count = 0;
    for (const GlucoseScheduleRow& row : settings.schedule.rows) {
        if (count >= capacity) {
            break;
        }
        out[count].minutes = row.minutes;
        out[count].face = apps::glucoseFaceFromName(row.face);
        out[count].brightness = row.brightness;
        ++count;
    }
    return count;
}

std::string glucoseFaceMembersJson(const GlucoseSettings& s) {
    std::string out;
    out.reserve(256);
    out += "\"face\":\"";
    out += s.face;  // a validated face name: no escaping to do
    out += "\",\"faces\":[";
    for (std::size_t i = 0; i < s.faces.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += '"';
        out += s.faces[i];
        out += '"';
    }
    out += "],\"cycleSeconds\":";
    out += std::to_string(s.cycleSeconds);
    out += ",\"schedule\":{\"enabled\":";
    out += s.schedule.enabled ? "true" : "false";
    out += ",\"rows\":[";
    for (std::size_t i = 0; i < s.schedule.rows.size(); ++i) {
        const GlucoseScheduleRow& row = s.schedule.rows[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"from\":";
        appendTime(out, row.minutes);
        out += ",\"face\":\"";
        out += row.face;
        out += "\",\"brightness\":";
        out += row.brightness < 0 ? std::string("null") : std::to_string(row.brightness);
        out += '}';
    }
    out += "]}";
    return out;
}

void loadGlucoseFaceSettings(const json::Value& glucose, GlucoseSettings& out) {
    // faces: keep the known names, in canonical order, once each.
    if (const json::Value faces = glucose["faces"]; faces.isArray()) {
        plan::FaceMask mask = 0;
        for (int i = 0; i < faces.size(); ++i) {
            GlucoseFace face = GlucoseFace::Hero;
            if (faces[i].isString() && selectableName(faces[i].raw(), face)) {
                mask = static_cast<plan::FaceMask>(mask | plan::faceBit(face));
            }
        }
        out.faces = namesOf(mask == 0 ? plan::kAllFaces : mask);
    }
    const plan::FaceMask mask = faceMaskOf(out);

    // The default: an unknown or unused one becomes the first face in use.
    GlucoseFace face = GlucoseFace::Hero;
    if (!selectableName(out.face, face) || !plan::faceActive(mask, face)) {
        out.face = apps::glucoseFaceName(plan::firstActiveFace(mask));
    }

    const std::int64_t cycle = glucose["cycleSeconds"].toInt(out.cycleSeconds);
    out.cycleSeconds = plan::cycleSecondsAllowed(static_cast<int>(cycle)) &&
                               (cycle == 0 || plan::activeFaceCount(mask) >= 2)
                           ? static_cast<int>(cycle)
                           : 0;

    const json::Value schedule = glucose["schedule"];
    if (schedule.isObject()) {
        out.schedule.rows.clear();
        const json::Value rows = schedule["rows"];
        for (int i = 0; rows.isArray() && i < rows.size(); ++i) {
            if (out.schedule.rows.size() >= plan::kMaxScheduleRows) {
                break;
            }
            const json::Value row = rows[i];
            GlucoseScheduleRow parsed;
            GlucoseFace rowFace = GlucoseFace::Hero;
            if (!readTime(row["from"], parsed.minutes) || !row["face"].isString() ||
                !selectableName(row["face"].raw(), rowFace) || !plan::faceActive(mask, rowFace)) {
                continue;  // a row that cannot be honoured is dropped, not guessed
            }
            parsed.face = apps::glucoseFaceName(rowFace);
            const std::int64_t level = row["brightness"].toInt(-1);
            parsed.brightness = level >= 0 && level <= 255 ? static_cast<int>(level) : -1;
            const bool duplicate = std::any_of(
                out.schedule.rows.begin(), out.schedule.rows.end(),
                [&](const GlucoseScheduleRow& r) { return r.minutes == parsed.minutes; });
            if (!duplicate) {
                out.schedule.rows.push_back(parsed);
            }
        }
        sortRows(out.schedule.rows);
        out.schedule.enabled =
            schedule["enabled"].toBool(false) && !out.schedule.rows.empty();
    }
}

bool applyGlucoseFaceSettings(const json::Value& glucose, GlucoseSettings& out,
                              std::string& error) {
    bool faceGiven = false;
    if (const json::Value value = glucose["face"]; value.valid()) {
        GlucoseFace face = GlucoseFace::Hero;
        if (!value.isString() || !selectableName(value.raw(), face)) {
            error = "'glucose.face' is not a known face";
            return false;
        }
        out.face = apps::glucoseFaceName(face);
        faceGiven = true;
    }

    if (const json::Value value = glucose["faces"]; value.valid()) {
        if (!value.isArray()) {
            error = "'glucose.faces' must be a list of face names";
            return false;
        }
        plan::FaceMask mask = 0;
        for (int i = 0; i < value.size(); ++i) {
            GlucoseFace face = GlucoseFace::Hero;
            if (!value[i].isString() || !selectableName(value[i].raw(), face)) {
                error = "'glucose.faces' names a face that does not exist";
                return false;
            }
            mask = static_cast<plan::FaceMask>(mask | plan::faceBit(face));
        }
        if (mask == 0) {
            error = "'glucose.faces' must name at least one face";
            return false;
        }
        out.faces = namesOf(mask);
        GlucoseFace current = GlucoseFace::Hero;
        if (!faceGiven &&
            (!selectableName(out.face, current) || !plan::faceActive(mask, current))) {
            out.face = apps::glucoseFaceName(plan::firstActiveFace(mask));
        }
    }

    if (const json::Value value = glucose["cycleSeconds"]; value.valid()) {
        std::int64_t seconds = 0;
        if (!readWhole(value, seconds) || !plan::cycleSecondsAllowed(static_cast<int>(seconds))) {
            error = "'glucose.cycleSeconds' must be 0, 10, 30, 60, 120, 180 or 300";
            return false;
        }
        out.cycleSeconds = static_cast<int>(seconds);
    }

    if (const json::Value schedule = glucose["schedule"]; schedule.valid()) {
        if (!schedule.isObject()) {
            error = "'glucose.schedule' must be an object";
            return false;
        }
        if (const json::Value enabled = schedule["enabled"]; enabled.valid()) {
            if (!enabled.isBoolean()) {
                error = "'glucose.schedule.enabled' must be true or false";
                return false;
            }
            out.schedule.enabled = enabled.toBool(false);
        }
        if (const json::Value rows = schedule["rows"]; rows.valid()) {
            if (!rows.isArray()) {
                error = "'glucose.schedule.rows' must be a list";
                return false;
            }
            if (rows.size() > static_cast<int>(plan::kMaxScheduleRows)) {
                error = "'glucose.schedule.rows' holds at most six rows";
                return false;
            }
            std::vector<GlucoseScheduleRow> parsed;
            for (int i = 0; i < rows.size(); ++i) {
                const json::Value row = rows[i];
                GlucoseScheduleRow next;
                if (!row.isObject() || !readTime(row["from"], next.minutes)) {
                    error = "'glucose.schedule.rows' each need 'from' as \"HH:MM\"";
                    return false;
                }
                GlucoseFace face = GlucoseFace::Hero;
                if (!row["face"].isString() || !selectableName(row["face"].raw(), face)) {
                    error = "'glucose.schedule.rows' names a face that does not exist";
                    return false;
                }
                next.face = apps::glucoseFaceName(face);
                const json::Value level = row["brightness"];
                if (level.valid() && !level.isNull()) {
                    std::int64_t value = 0;
                    if (!readWhole(level, value) || value < 0 || value > 255) {
                        error = "'glucose.schedule.rows' brightness must be 0-255 or null";
                        return false;
                    }
                    next.brightness = static_cast<int>(value);
                }
                parsed.push_back(next);
            }
            sortRows(parsed);
            out.schedule.rows = std::move(parsed);
        }
    }

    return checkWhole(out, error);
}

}  // namespace config
}  // namespace stipple
