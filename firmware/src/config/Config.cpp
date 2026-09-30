// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/config/Config.h"

#include "stipple/core/Checksum.h"
#include "stipple/core/Rgb.h"
#include "stipple/json/Json.h"

namespace stipple {
namespace config {
namespace {

std::uint8_t clampToByte(std::int64_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

/// 0-1439. A time of day cannot be outside a day, and a stored document that
/// says otherwise is corrupt rather than interesting.
int clampMinutes(std::int64_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 1439) {
        return 1439;
    }
    return static_cast<int>(value);
}

int clampDuration(std::int64_t value) noexcept {
    if (value < 1) {
        return 1;
    }
    if (value > 3600) {
        return 3600;
    }
    return static_cast<int>(value);
}

int clampPort(std::int64_t value) noexcept {
    if (value < 1) {
        return 1883;
    }
    return value > 65535 ? 65535 : static_cast<int>(value);
}

/// A glucose source is asked once a minute by default; under 30 s is hammering
/// somebody's Nightscout, over 10 min is a reading that goes stale between asks.
int clampPollSeconds(std::int64_t value) noexcept {
    if (value < 30) {
        return 30;
    }
    return value > 600 ? 600 : static_cast<int>(value);
}

/// MQTT allows up to 18 hours; anything under 5 seconds is a keepalive storm.
int clampKeepAlive(std::int64_t value) noexcept {
    if (value < 5) {
        return 5;
    }
    return value > 65535 ? 65535 : static_cast<int>(value);
}

std::uint8_t clampPercent(std::int64_t value) noexcept {
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return static_cast<std::uint8_t>(value);
}

/// 0 means "hold the colon lit", so it has to survive clamping. Anything faster
/// than 100 ms is a strobe rather than a blink, and the ceiling keeps the
/// scheduler's next-due arithmetic in comfortable range.
std::uint32_t clampBlinkPeriod(std::int64_t value) noexcept {
    if (value <= 0) {
        return 0;
    }
    if (value < 100) {
        return 100;
    }
    if (value > 60000) {
        return 60000;
    }
    return static_cast<std::uint32_t>(value);
}

int clampUtcOffset(std::int64_t value) noexcept {
    // Real zones span UTC-12 to UTC+14.
    if (value < -12 * 3600) {
        return -12 * 3600;
    }
    if (value > 14 * 3600) {
        return 14 * 3600;
    }
    return static_cast<int>(value);
}

void appendEscaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xFu]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xFu]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

std::string buildBody(const Config& config) {
    std::string body = "{\"schemaVersion\":";
    body += std::to_string(kCurrentSchemaVersion);

    body += ",\"deviceName\":";
    appendEscaped(body, config.deviceName);

    body += ",\"display\":{\"brightness\":";
    body += std::to_string(static_cast<int>(config.display.brightness));
    body += ",\"power\":";
    body += config.display.power ? "true" : "false";
    body += ",\"overlay\":";
    appendEscaped(body, config.display.overlay);
    body += ",\"night\":{\"enabled\":";
    body += config.display.night.enabled ? "true" : "false";
    body += ",\"startMinutes\":";
    body += std::to_string(config.display.night.startMinutes);
    body += ",\"endMinutes\":";
    body += std::to_string(config.display.night.endMinutes);
    body += ",\"brightness\":";
    body += std::to_string(static_cast<int>(config.display.night.brightness));
    body += "}}";

    body += ",\"audio\":{\"volumePercent\":";
    body += std::to_string(static_cast<int>(config.audio.volumePercent));
    body += '}';

    body += ",\"mqtt\":{\"enabled\":";
    body += config.mqtt.enabled ? "true" : "false";
    body += ",\"host\":";
    appendEscaped(body, config.mqtt.host);
    body += ",\"port\":";
    body += std::to_string(config.mqtt.port);
    body += ",\"clientId\":";
    appendEscaped(body, config.mqtt.clientId);
    body += ",\"baseTopic\":";
    appendEscaped(body, config.mqtt.baseTopic);
    body += ",\"username\":";
    appendEscaped(body, config.mqtt.username);
    // The one place a credential is written down. It has to be here - the device
    // reconnects unattended - but it must never reach the API or the log (§22).
    body += ",\"password\":";
    appendEscaped(body, config.mqtt.password);
    body += ",\"tls\":";
    body += config.mqtt.tls ? "true" : "false";
    body += ",\"keepAliveSeconds\":";
    body += std::to_string(config.mqtt.keepAliveSeconds);
    body += ",\"discovery\":";
    body += config.mqtt.discovery ? "true" : "false";
    body += '}';

    body += ",\"apps\":{\"defaultDurationSeconds\":";
    body += std::to_string(config.apps.defaultDurationSeconds);
    body += ",\"transitions\":";
    body += config.apps.transitions ? "true" : "false";
    body += ",\"autoAdvance\":";
    body += config.apps.autoAdvance ? "true" : "false";
    body += ",\"transition\":";
    appendEscaped(body, config.apps.transition);
    body += ",\"order\":[";
    for (std::size_t i = 0; i < config.apps.order.size(); ++i) {
        if (i > 0) {
            body += ',';
        }
        const config::AppPreference& preference = config.apps.order[i];
        body += "{\"id\":";
        appendEscaped(body, preference.id);
        body += ",\"enabled\":";
        body += preference.enabled ? "true" : "false";
        body += ",\"durationSeconds\":";
        body += std::to_string(preference.durationSeconds);
        body += '}';
    }
    body += "]}";

    body += ",\"clock\":{\"twentyFourHour\":";
    body += config.clock.twentyFourHour ? "true" : "false";
    body += ",\"utcOffsetSeconds\":";
    body += std::to_string(config.clock.utcOffsetSeconds);
    body += ",\"theme\":";
    appendEscaped(body, config.clock.theme);
    body += ",\"timezone\":";
    appendEscaped(body, config.clock.timezone);
    body += ",\"ntpServer\":";
    appendEscaped(body, config.clock.ntpServer);
    body += ",\"leadingZero\":";
    body += config.clock.leadingZero ? "true" : "false";
    body += ",\"showAmPm\":";
    body += config.clock.showAmPm ? "true" : "false";

    // Colours go out as #RRGGBB so a stored document stays readable by whoever
    // has to debug one over ADB.
    char hex[8];
    formatHexColor(fromPacked(config.clock.color), hex);
    body += ",\"color\":";
    appendEscaped(body, hex);
    formatHexColor(fromPacked(config.clock.accentColor), hex);
    body += ",\"accentColor\":";
    appendEscaped(body, hex);
    formatHexColor(fromPacked(config.clock.dateColor), hex);
    body += ",\"dateColor\":";
    appendEscaped(body, hex);

    body += ",\"dateOrder\":";
    appendEscaped(body, config.clock.dateOrder);
    body += ",\"dateSeparator\":";
    appendEscaped(body, config.clock.dateSeparator);
    body += ",\"dateYear\":";
    appendEscaped(body, config.clock.dateYear);
    body += ",\"blinkPeriodMillis\":";
    body += std::to_string(config.clock.blinkPeriodMillis);
    body += ",\"tick\":";
    body += config.clock.tick ? "true" : "false";
    body += '}';

    body += ",\"web\":{\"username\":";
    appendEscaped(body, config.web.username);
    body += ",\"password\":";
    appendEscaped(body, config.web.password);
    body += '}';

    body += ",\"network\":{\"hotspotRequested\":";
    body += config.network.hotspotRequested ? "true" : "false";
    body += '}';

    body += ",\"notifications\":{\"sound\":";
    appendEscaped(body, config.notifications.sound);
    body += '}';

    body += ",\"visualizer\":{\"style\":";
    appendEscaped(body, config.visualizer.style);
    body += '}';

    body += ",\"glucose\":{\"url\":";
    appendEscaped(body, config.glucose.url);
    body += ",\"apiSecretSha1\":";
    appendEscaped(body, config.glucose.apiSecretSha1);
    body += ",\"pollSeconds\":";
    body += std::to_string(config.glucose.pollSeconds);
    body += ",\"face\":";
    appendEscaped(body, config.glucose.face);
    body += ",\"pinned\":";
    body += config.glucose.pinned ? "true" : "false";
    body += '}';

    body += '}';
    return body;
}

}  // namespace

const char* describe(LoadStatus status) noexcept {
    switch (status) {
        case LoadStatus::Loaded: return "loaded";
        case LoadStatus::Migrated: return "migrated from an older schema";
        case LoadStatus::RecoveredFromBackup: return "recovered from backup";
        case LoadStatus::DefaultsMissing: return "no configuration stored; using defaults";
        case LoadStatus::DefaultsCorrupt: return "configuration corrupt; using defaults";
        case LoadStatus::DefaultsFutureSchema:
            return "configuration written by newer firmware; using defaults";
    }
    return "unknown";
}

std::string ConfigStore::serialize(const Config& config) {
    const std::string body = buildBody(config);

    char hex[9];
    crc32ToHex(crc32(body), hex);

    std::string payload = "{\"checksum\":\"";
    payload += hex;
    payload += "\",\"body\":";
    payload += body;
    payload += '}';
    return payload;
}

bool ConfigStore::deserialize(std::string_view payload,
                              Config& out,
                              int& fromSchemaVersion,
                              bool& futureSchema) {
    fromSchemaVersion = 0;
    futureSchema = false;

    json::Token tokens[kMaxTokens];
    json::Document document(tokens, kMaxTokens);
    if (document.parse(payload) != json::Error::None) {
        return false;
    }

    const json::Value root = document.root();
    const json::Value checksum = root["checksum"];
    const json::Value body = root["body"];
    if (!checksum.isString() || !body.isObject()) {
        return false;
    }

    // The checksum covers the body text byte for byte, so a truncated or
    // partially-rewritten record is rejected rather than half-applied.
    const std::string expected = checksum.toString();
    char actual[9];
    crc32ToHex(crc32(body.raw()), actual);
    if (expected != actual) {
        return false;
    }

    const json::Value version = body["schemaVersion"];
    if (!version.isNumber()) {
        return false;
    }
    const std::int64_t schemaVersion = version.toInt(0);
    if (schemaVersion < 1) {
        return false;
    }
    if (schemaVersion > kCurrentSchemaVersion) {
        fromSchemaVersion = static_cast<int>(schemaVersion);
        futureSchema = true;
        return false;
    }
    fromSchemaVersion = static_cast<int>(schemaVersion);

    Config parsed;
    parsed.schemaVersion = kCurrentSchemaVersion;
    parsed.deviceName = body["deviceName"].toString(parsed.deviceName);

    const json::Value display = body["display"];
    const std::int64_t rawBrightness =
        display["brightness"].toInt(static_cast<std::int64_t>(parsed.display.brightness));

    // Migration v1 -> v2: brightness used to be a percentage.
    parsed.display.brightness = fromSchemaVersion < 2
                                    ? clampToByte((rawBrightness * 255 + 50) / 100)
                                    : clampToByte(rawBrightness);
    parsed.display.power = display["power"].toBool(parsed.display.power);

    const json::Value night = display["night"];
    parsed.display.night.enabled = night["enabled"].toBool(parsed.display.night.enabled);
    parsed.display.night.startMinutes =
        clampMinutes(night["startMinutes"].toInt(parsed.display.night.startMinutes));
    parsed.display.night.endMinutes =
        clampMinutes(night["endMinutes"].toInt(parsed.display.night.endMinutes));
    parsed.display.night.brightness = clampToByte(
        night["brightness"].toInt(static_cast<std::int64_t>(parsed.display.night.brightness)));
    parsed.display.overlay = display["overlay"].toString(parsed.display.overlay);

    const json::Value audio = body["audio"];
    parsed.audio.volumePercent =
        clampPercent(audio["volumePercent"].toInt(parsed.audio.volumePercent));

    const json::Value mqtt = body["mqtt"];
    parsed.mqtt.enabled = mqtt["enabled"].toBool(parsed.mqtt.enabled);
    parsed.mqtt.host = mqtt["host"].toString(parsed.mqtt.host);
    parsed.mqtt.port = clampPort(mqtt["port"].toInt(parsed.mqtt.port));
    parsed.mqtt.clientId = mqtt["clientId"].toString(parsed.mqtt.clientId);
    parsed.mqtt.baseTopic = mqtt["baseTopic"].toString(parsed.mqtt.baseTopic);
    parsed.mqtt.username = mqtt["username"].toString(parsed.mqtt.username);
    parsed.mqtt.password = mqtt["password"].toString(parsed.mqtt.password);
    parsed.mqtt.tls = mqtt["tls"].toBool(parsed.mqtt.tls);
    parsed.mqtt.keepAliveSeconds =
        clampKeepAlive(mqtt["keepAliveSeconds"].toInt(parsed.mqtt.keepAliveSeconds));
    parsed.mqtt.discovery = mqtt["discovery"].toBool(parsed.mqtt.discovery);

    // An empty base topic would publish to "/{deviceId}/status" - a leading
    // slash is legal MQTT but a well-known source of confusion, so fall back.
    if (parsed.mqtt.baseTopic.empty()) {
        parsed.mqtt.baseTopic = "stipple";
    }

    const json::Value apps = body["apps"];
    parsed.apps.defaultDurationSeconds = clampDuration(
        apps["defaultDurationSeconds"].toInt(parsed.apps.defaultDurationSeconds));
    parsed.apps.transitions = apps["transitions"].toBool(parsed.apps.transitions);
    parsed.apps.autoAdvance = apps["autoAdvance"].toBool(parsed.apps.autoAdvance);
    parsed.apps.transition = apps["transition"].toString(parsed.apps.transition);

    // An order that cannot be read is dropped, not fatal. Losing the
    // arrangement of a carousel is a small annoyance; refusing to boot over it
    // is not, and this whole file exists so one bad field cannot cost the user
    // every other setting they have.
    parsed.apps.order.clear();
    if (const json::Value order = apps["order"]; order.isArray()) {
        const int count = order.size();
        for (int i = 0; i < count; ++i) {
            const json::Value entry = order[i];
            if (!entry.isObject()) {
                continue;
            }
            AppPreference preference;
            preference.id = entry["id"].toString(std::string());
            if (preference.id.empty()) {
                continue;  // an entry naming nothing orders nothing
            }
            preference.enabled = entry["enabled"].toBool(true);
            preference.durationSeconds = static_cast<int>(entry["durationSeconds"].toInt(0));
            if (preference.durationSeconds < 0) {
                preference.durationSeconds = 0;
            }
            parsed.apps.order.push_back(std::move(preference));
            // Bounded like the registry it mirrors: a stored document must not
            // be able to make this grow without limit, and an order longer
            // than the registry can hold describes apps that cannot exist.
            if (parsed.apps.order.size() >= static_cast<std::size_t>(kMaxRememberedApps)) {
                break;
            }
        }
    }

    const json::Value web = body["web"];
    parsed.web.username = web["username"].toString(parsed.web.username);
    parsed.web.password = web["password"].toString(parsed.web.password);

    const json::Value network = body["network"];
    parsed.network.hotspotRequested =
        network["hotspotRequested"].toBool(parsed.network.hotspotRequested);

    const json::Value notifications = body["notifications"];
    parsed.notifications.sound =
        notifications["sound"].toString(parsed.notifications.sound);

    const json::Value visualizer = body["visualizer"];
    parsed.visualizer.style = visualizer["style"].toString(parsed.visualizer.style);

    const json::Value glucose = body["glucose"];
    parsed.glucose.url = glucose["url"].toString(parsed.glucose.url);
    parsed.glucose.apiSecretSha1 = glucose["apiSecretSha1"].toString(parsed.glucose.apiSecretSha1);
    parsed.glucose.pollSeconds =
        clampPollSeconds(glucose["pollSeconds"].toInt(parsed.glucose.pollSeconds));
    parsed.glucose.face = glucose["face"].toString(parsed.glucose.face);
    parsed.glucose.pinned = glucose["pinned"].toBool(parsed.glucose.pinned);

    const json::Value clock = body["clock"];
    parsed.clock.twentyFourHour = clock["twentyFourHour"].toBool(parsed.clock.twentyFourHour);
    parsed.clock.utcOffsetSeconds =
        clampUtcOffset(clock["utcOffsetSeconds"].toInt(parsed.clock.utcOffsetSeconds));
    parsed.clock.theme = clock["theme"].toString(parsed.clock.theme);
    parsed.clock.timezone = clock["timezone"].toString(parsed.clock.timezone);
    parsed.clock.ntpServer = clock["ntpServer"].toString(parsed.clock.ntpServer);
    parsed.clock.leadingZero = clock["leadingZero"].toBool(parsed.clock.leadingZero);
    parsed.clock.showAmPm = clock["showAmPm"].toBool(parsed.clock.showAmPm);
    parsed.clock.tick = clock["tick"].toBool(parsed.clock.tick);

    // A colour that will not parse keeps the default rather than failing the
    // load. Configuration recovery exists so one bad field cannot cost the user
    // every other setting they have.
    const auto colorOr = [&clock](const char* key, std::uint32_t fallback) {
        Rgb parsedColor;
        const json::Value value = clock[key];
        if (value.isString() && parseHexColor(value.raw(), parsedColor)) {
            return toPacked(parsedColor);
        }
        return fallback;
    };
    parsed.clock.color = colorOr("color", parsed.clock.color);
    parsed.clock.accentColor = colorOr("accentColor", parsed.clock.accentColor);
    parsed.clock.dateColor = colorOr("dateColor", parsed.clock.dateColor);

    parsed.clock.dateOrder = clock["dateOrder"].toString(parsed.clock.dateOrder);
    parsed.clock.dateSeparator = clock["dateSeparator"].toString(parsed.clock.dateSeparator);
    parsed.clock.dateYear = clock["dateYear"].toString(parsed.clock.dateYear);
    parsed.clock.blinkPeriodMillis =
        clampBlinkPeriod(clock["blinkPeriodMillis"].toInt(parsed.clock.blinkPeriodMillis));

    out = std::move(parsed);
    return true;
}

LoadReport ConfigStore::load(Config& out) const {
    LoadReport report;

    std::string payload;
    const bool primaryExists = storage_.read(kPrimaryKey, payload);

    if (primaryExists) {
        Config parsed;
        int version = 0;
        bool future = false;
        if (deserialize(payload, parsed, version, future)) {
            out = std::move(parsed);
            report.fromSchemaVersion = version;
            report.status =
                version < kCurrentSchemaVersion ? LoadStatus::Migrated : LoadStatus::Loaded;
            return report;
        }
        report.fromSchemaVersion = version;
        report.status = future ? LoadStatus::DefaultsFutureSchema : LoadStatus::DefaultsCorrupt;
    } else {
        report.status = LoadStatus::DefaultsMissing;
    }

    // The primary is unusable. The backup holds the last value that was good
    // enough to be replaced, which is the whole point of writing it.
    std::string backup;
    if (storage_.read(kBackupKey, backup)) {
        Config parsed;
        int version = 0;
        bool future = false;
        if (deserialize(backup, parsed, version, future)) {
            out = std::move(parsed);
            report.status = LoadStatus::RecoveredFromBackup;
            report.fromSchemaVersion = version;
            report.usedBackup = true;
            return report;
        }
    }

    out = Config{};
    return report;
}

bool ConfigStore::save(const Config& config) {
    const std::string payload = serialize(config);
    if (payload.size() > storage_.maxValueBytes()) {
        return false;
    }

    // Back up the current value first. IStorage guarantees each write is atomic,
    // so a power cut leaves either the old primary intact or the new one
    // complete, with the previous value still under the backup key either way.
    std::string current;
    if (storage_.read(kPrimaryKey, current) && current != payload) {
        storage_.write(kBackupKey, current);
    }

    return storage_.write(kPrimaryKey, payload);
}

}  // namespace config
}  // namespace stipple
