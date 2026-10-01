// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/api/ApiServer.h"

#include "stipple/platform/MqttClient.h"
#include "stipple/update/ElfCheck.h"

#include "stipple/update/UpdateImage.h"

#include <vector>

#include "stipple/api/JsonWriter.h"
#include "stipple/audio/Sound.h"
#include "stipple/app/AppRegistry.h"
#include "stipple/core/Base64.h"
#include "stipple/render/FrameScheduler.h"
#include "stipple/render/Transition.h"
#include "stipple/time/Timezone.h"
#include "stipple/render/Overlay.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/asset/IconStore.h"
#include "stipple/scene/Scene.h"
#include "stipple/script/IScriptRunner.h"
#include "stipple/app/Carousel.h"
#include "stipple/apps/ClockApp.h"
#include "stipple/apps/GlucoseAlarm.h"
#include "stipple/apps/GlucoseApp.h"
#include "stipple/audio/Melody.h"
#include "stipple/apps/GlucoseSource.h"
#include "stipple/apps/VisualizerApp.h"
#include "stipple/config/Config.h"
#include "stipple/config/GlucoseFaceSettings.h"
#include "stipple/config/GlucoseSourceSettings.h"
#include "stipple/core/Sha1.h"
#include "stipple/net/HttpFetch.h"
#include "stipple/core/Log.h"
#include "stipple/core/Version.h"
#include "stipple/json/Json.h"
#include "stipple/notify/Notifications.h"
#include "stipple/platform/PlatformServices.h"

namespace stipple {
namespace api {
namespace {

/// Bounds on an inline tone from the network.
///
/// The same ceilings a script gets, deliberately: there is no reason the API
/// should reach further into the speaker than the device's own code does. The
/// floor is there because a 1 Hz "tone" is not audible, it is just the
/// speaker being held for a second.
constexpr std::int64_t kMinToneHz = 50;
constexpr std::int64_t kMaxToneHz = 8000;
constexpr std::int64_t kMaxToneMillis = 5000;
constexpr std::int64_t kDefaultToneMillis = 150;

/// A parsed request body, owning its token storage.
///
/// Tokens are heap-allocated because the budget is configurable, but they are
/// bounded by ApiOptions and freed as soon as the request is answered. This is
/// request-handling, not the render path, so the allocation rule that governs
/// rendering does not apply here.
class Body {
public:
    Body(std::string_view text, int maxTokens, std::size_t maxBytes)
        : tokens_(static_cast<std::size_t>(maxTokens > 0 ? maxTokens : 1)),
          document_(tokens_.data(), static_cast<int>(tokens_.size())) {
        json::Limits limits;
        limits.maxInputBytes = maxBytes;
        error_ = document_.parse(text, limits);
    }

    bool valid() const { return error_ == json::Error::None; }
    const char* errorText() const { return json::describe(error_); }
    json::Value root() const { return document_.root(); }

private:
    std::vector<json::Token> tokens_;
    json::Document document_;
    json::Error error_ = json::Error::None;
};

void writeApp(JsonWriter& writer, const app::App& entry, int position) {
    writer.beginObject()
        .member("id", entry.id)
        .member("name", entry.name)
        .member("enabled", entry.enabled)
        .member("position", position)
        .member("durationSeconds", entry.durationSeconds)
        .member("source", app::appSourceName(entry.source));
    // Stored scenes are validated on the way in, so embedding them verbatim
    // avoids a needless parse-and-reserialise round trip.
    writer.rawMember("scene", entry.sceneJson);
    writer.endObject();
}

void writeNotification(JsonWriter& writer, const notify::Notification& notification) {
    writer.beginObject()
        .member("id", notification.id)
        .member("text", notification.text)
        .member("priority", notify::priorityName(notification.priority))
        .member("durationSeconds", notification.durationSeconds)
        .member("hold", notification.hold)
        .member("dismissible", notification.dismissible)
        .member("icon", notification.icon)
        .endObject();
}

void writeSettings(JsonWriter& writer, const config::Config& settings) {
    writer.beginObject()
        .member("schemaVersion", settings.schemaVersion)
        .member("deviceName", settings.deviceName)
        .key("display")
        .beginObject()
        .member("brightness", static_cast<int>(settings.display.brightness))
        .member("power", settings.display.power)
        .member("overlay", settings.display.overlay)
        .key("night")
        .beginObject()
        .member("enabled", settings.display.night.enabled)
        .member("startMinutes", settings.display.night.startMinutes)
        .member("endMinutes", settings.display.night.endMinutes)
        .member("brightness", static_cast<int>(settings.display.night.brightness))
        .endObject()
        .endObject()
        .key("audio")
        .beginObject()
        .member("volumePercent", static_cast<int>(settings.audio.volumePercent))
        .endObject()
        .key("mqtt")
        .beginObject()
        .member("enabled", settings.mqtt.enabled)
        .member("host", settings.mqtt.host)
        .member("port", settings.mqtt.port)
        .member("clientId", settings.mqtt.clientId)
        .member("baseTopic", settings.mqtt.baseTopic)
        .member("username", settings.mqtt.username)
        // The password is never returned (§22). A boolean says whether one is
        // set, so a settings page can show "configured" without the value, and
        // without a masked placeholder that a client might helpfully save back.
        .member("passwordSet", !settings.mqtt.password.empty())
        .member("tls", settings.mqtt.tls)
        .member("keepAliveSeconds", settings.mqtt.keepAliveSeconds)
        .member("discovery", settings.mqtt.discovery)
        .endObject()
        .key("web")
        .beginObject()
        // An empty username means no authentication at all, which is the
        // default. Reported so a page can say so plainly rather than leaving
        // somebody to infer it from two blank fields.
        .member("username", settings.web.username)
        // Never returned, exactly like the MQTT password - which also keeps
        // it out of backups, since those are taken from this API. A settings
        // file in somebody's downloads folder should not be a credential.
        .member("passwordSet", !settings.web.password.empty())
        .endObject()
        .key("apps")
        .beginObject()
        .member("defaultDurationSeconds", settings.apps.defaultDurationSeconds)
        .member("transitions", settings.apps.transitions)
        .member("autoAdvance", settings.apps.autoAdvance)
        .member("transition", settings.apps.transition);

    // The arrangement, which is a setting like any other.
    //
    // It was stored to flash and left out of this document, so a backup
    // silently omitted the app order and a restore could not bring it back -
    // a field that persists but cannot be read is a field nobody can save.
    writer.key("order").beginArray();
    for (const config::AppPreference& preference : settings.apps.order) {
        writer.beginObject()
            .member("id", preference.id)
            .member("enabled", preference.enabled)
            .member("durationSeconds", preference.durationSeconds)
            .endObject();
    }
    writer.endArray();

    writer.endObject()
        .key("clock")
        .beginObject()
        .member("twentyFourHour", settings.clock.twentyFourHour)
        .member("utcOffsetSeconds", settings.clock.utcOffsetSeconds)
        .member("theme", settings.clock.theme)
        .member("timezone", settings.clock.timezone)
        .member("ntpServer", settings.clock.ntpServer)
        .member("leadingZero", settings.clock.leadingZero)
        .member("showAmPm", settings.clock.showAmPm);

    char hex[8];
    formatHexColor(fromPacked(settings.clock.color), hex);
    writer.member("color", hex);
    formatHexColor(fromPacked(settings.clock.accentColor), hex);
    writer.member("accentColor", hex);
    formatHexColor(fromPacked(settings.clock.dateColor), hex);
    writer.member("dateColor", hex);

    writer.member("dateOrder", settings.clock.dateOrder)
        .member("dateSeparator", settings.clock.dateSeparator)
        .member("dateYear", settings.clock.dateYear)
        .member("blinkPeriodMillis", static_cast<int>(settings.clock.blinkPeriodMillis))
        .member("tick", settings.clock.tick)
        .endObject()
        .key("notifications")
        .beginObject()
        .member("sound", settings.notifications.sound)
        .endObject()
        .key("visualizer")
        .beginObject()
        .member("style", settings.visualizer.style)
        .endObject()
        .key("glucose")
        .beginObject()
        .member("url", settings.glucose.url)
        // The credential is never returned, not even as its hash - the hash
        // is what the server accepts, so it is the credential. A boolean says
        // whether one is set (§22, the MQTT password rule).
        .member("apiSecretSet", !settings.glucose.apiSecretSha1.empty())
        .member("pollSeconds", static_cast<std::int64_t>(settings.glucose.pollSeconds))
        .rawMembers(config::glucoseFaceMembersJson(settings.glucose))
        .rawMembers(config::glucoseSourcePublicMembers(settings.glucose))
        .member("pinned", settings.glucose.pinned)
        .rawMember("alarms", config::alarmSettingsJson(settings.glucose.alarms))
        .endObject()
        .endObject();
}

}  // namespace

bool ApiServer::authorised(const Request& request) const {
    if (options_.authToken.empty()) {
        return true;
    }
    return request.authToken == options_.authToken;
}

Response ApiServer::handle(const Request& request, std::uint64_t nowMillis) {
    // Size is checked before anything looks at the body, so an oversized
    // payload costs a length comparison rather than a parse.
    //
    // A firmware image is the one thing that legitimately dwarfs every other
    // request, so it gets its own ceiling rather than raising the general
    // one - which would let any request allocate megabytes on a device with
    // 36 MB of RAM.
    const Resource sized = matchRoute(request.path).resource;
    std::size_t bodyCeiling = options_.maxBodyBytes;
    if (sized == Resource::SystemFirmware) {
        bodyCeiling = options_.maxImageBytes;
    } else if (sized == Resource::AssetCollection) {
        // An icon is pixels as JSON integers, roughly nine bytes each. A
        // 32x32 animation is well past the general limit and entirely
        // reasonable, so it gets its own ceiling rather than raising one that
        // every other route would inherit.
        bodyCeiling = options_.maxIconBytes;
    }
    if (request.body.size() > bodyCeiling) {
        return payloadTooLarge();
    }

    if (request.method == Method::Unknown) {
        return badRequest("unsupported HTTP method");
    }

    const RouteMatch route = matchRoute(request.path);
    if (route.resource == Resource::Unknown) {
        // A request under /api/ but outside /api/v1/ is asking for an API
        // version this device does not serve. Naming the one it does serve
        // turns a dead end into something the caller can act on.
        const std::string versionedPrefix = std::string(kApiV1Prefix) + "/";
        const bool underApi = request.path.rfind("/api/", 0) == 0;
        const bool underCurrentVersion =
            request.path == kApiV1Prefix || request.path.rfind(versionedPrefix, 0) == 0;

        if (underApi && !underCurrentVersion) {
            return notFound("unknown API version; this device serves " +
                            std::string(kApiV1Prefix) + " only");
        }
        return notFound("no such endpoint");
    }

    // Authentication is checked after routing so an unauthenticated caller
    // cannot use the 401/404 difference to enumerate valid endpoints.
    if (!authorised(request)) {
        return unauthorized();
    }

    switch (route.resource) {
        case Resource::Device: return handleDevice(request);
        case Resource::Health: return handleHealth(request, nowMillis);
        case Resource::Version: return handleVersion(request);
        case Resource::Diagnostics: return handleDiagnostics(request, nowMillis);
        case Resource::Logs: return handleLogs(request);
        case Resource::AppCollection: return handleAppCollection(request, nowMillis);
        case Resource::AppItem: return handleAppItem(request, route.id, nowMillis);
        case Resource::AppActivate: return handleAppActivate(request, route.id, nowMillis);
        case Resource::NotificationCollection:
            return handleNotificationCollection(request, nowMillis);
        case Resource::NotificationItem:
            return handleNotificationItem(request, route.id, nowMillis);
        case Resource::AssetCollection: return handleAssetCollection(request);
        case Resource::ScriptCollection: return handleScriptCollection(request);
        case Resource::ScriptItem: return handleScriptItem(request, route.id);
        case Resource::AssetItem: return handleAssetItem(request, route.id);
        case Resource::Settings: return handleSettings(request);
        case Resource::SystemReboot: return handleReboot(request);
        case Resource::SystemReset: return handleReset(request, nowMillis);
        case Resource::Network: return handleNetwork(request);
        case Resource::NetworkScan: return handleNetworkScan(request);
        case Resource::NetworkJoin: return handleNetworkJoin(request);
        case Resource::SystemFirmware: return handleFirmware(request);
        case Resource::DisplayFrame: return handleDisplayFrame(request);
        case Resource::Input: return handleInput(request, nowMillis);
        case Resource::GlucoseAlarmTest: return handleGlucoseAlarmTest(request, nowMillis);
        case Resource::Sound: return handleSound(request);
        case Resource::NetworkRemember: return handleNetworkRemember(request, true);
        case Resource::NetworkForget: return handleNetworkRemember(request, false);
        case Resource::Unknown: break;
    }
    return notFound("no such endpoint");
}

// --- live view ---------------------------------------------------------------

Response ApiServer::handleDisplayFrame(const Request& request) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }
    if (context_.frame == nullptr) {
        return notFound("this build does not expose the framebuffer");
    }

    // Raw RGB888, base64. Not PNG: stipple_imageio is deliberately absent from
    // the device build, and 2496 bytes is small enough that encoding anything
    // cleverer would cost more than it saved. The browser writes these straight
    // into an ImageData.
    const Framebuffer& frame = *context_.frame;

    JsonWriter writer;
    writer.beginObject()
        .member("width", Framebuffer::kWidth)
        .member("height", Framebuffer::kHeight)
        .member("format", "rgb888")
        .member("pixels", base64::encode(frame.bytes(), Framebuffer::kByteSize))
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleGlucoseAlarmTest(const Request& request, std::uint64_t nowMillis) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.glucoseAlarm == nullptr) {
        return notFound("this build has no glucose alarm");
    }
    platform::IAudioOutput* speaker =
        context_.platform != nullptr ? context_.platform->audio() : nullptr;
    if (speaker == nullptr) {
        return error(409, "no_speaker", "this device has no speaker");
    }
    // A test must never replace or talk over a real alarm.
    if (context_.glucoseAlarm->sounding()) {
        return error(409, "alarm_sounding", "an alarm is sounding; snooze it first");
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    // Either a melody as typed (the page's unsaved field), or a configured
    // alarm by name - what will actually sound.
    audio::Melody melody;
    if (const json::Value text = root["melody"]; text.isString()) {
        const audio::RtttlError parsed = audio::parseRtttl(text.toString(), melody);
        if (parsed != audio::RtttlError::None) {
            return unprocessable(std::string("'melody': ") + audio::describe(parsed));
        }
    } else if (const json::Value name = root["alarm"]; name.isString()) {
        apps::glucose::AlarmKind kind = apps::glucose::AlarmKind::None;
        for (const apps::glucose::AlarmKind each :
             {apps::glucose::AlarmKind::UrgentLow, apps::glucose::AlarmKind::Low,
              apps::glucose::AlarmKind::High, apps::glucose::AlarmKind::NoData}) {
            if (name.stringEquals(apps::glucose::alarmKindName(each))) {
                kind = each;
            }
        }
        if (kind == apps::glucose::AlarmKind::None) {
            return unprocessable("'alarm' must be urgentLow, low, high or noData");
        }
        melody = context_.glucoseAlarm->melodyFor(kind);
    } else {
        return unprocessable("give 'melody' (RTTTL) or 'alarm' (urgentLow, low, high, noData)");
    }

    if (!speaker->playMelody(melody, context_.glucoseAlarm->volumePercent())) {
        return serverError("the speaker refused the melody");
    }
    context_.glucoseAlarm->holdSpeaker(nowMillis +
                                       static_cast<std::uint64_t>(melody.totalMillis()) + 250u);

    JsonWriter writer;
    writer.beginObject()
        .member("playing", true)
        .member("millis", melody.totalMillis())
        .member("volumePercent", context_.glucoseAlarm->volumePercent())
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleInput(const Request& request, std::uint64_t nowMillis) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.input == nullptr) {
        return notFound("this build does not accept injected input");
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }

    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    const json::Value control = root["control"];
    if (!control.isString()) {
        return badRequest("'control' is required");
    }

    // Named for the labels on the case, matching RawInput. Anything else is
    // refused rather than mapped to a default: a web button that silently
    // pressed the wrong control would be worse than one that did nothing.
    const std::string name = control.toString();
    platform::RawInput source;
    if (name == "minus") source = platform::RawInput::KeyMinus;
    else if (name == "middle") source = platform::RawInput::KeyMiddle;
    else if (name == "plus") source = platform::RawInput::KeyPlus;
    else if (name == "press") source = platform::RawInput::RotaryPress;
    else if (name == "left") source = platform::RawInput::RotaryLeft;
    else if (name == "right") source = platform::RawInput::RotaryRight;
    else return unprocessable("'control' is not a known control");

    const bool rotation = source == platform::RawInput::RotaryLeft ||
                          source == platform::RawInput::RotaryRight;

    if (rotation) {
        // A detent has no duration; it arrives as a single Tick.
        platform::InputEvent event;
        event.source = source;
        event.phase = platform::ButtonPhase::Tick;
        event.timestampMillis = nowMillis;
        context_.input->inject(event);
        return noContent();
    }

    // Buttons arrive as a Down and an Up, because that is what the mapper
    // measures. holdMillis lets the web UI reach a long press, which is the
    // only way to trigger half the default bindings from a browser.
    std::uint64_t holdMillis = 0;
    if (const json::Value hold = root["holdMillis"]; hold.isNumber()) {
        const std::int64_t value = hold.toInt(0);
        if (value < 0 || value > 10000) {
            return unprocessable("'holdMillis' must be between 0 and 10000");
        }
        holdMillis = static_cast<std::uint64_t>(value);
    }

    // "phase" sends one half of a press, so a caller can hold a button down
    // across several requests.
    //
    // Without it the only thing reachable from outside is a complete press,
    // which makes any gesture involving two buttons at once untestable except
    // by standing in front of the device - and the rescue gesture is exactly
    // that, on the one path that has to work when nothing else does.
    if (const json::Value phase = root["phase"]; phase.isString()) {
        const std::string half = phase.toString();
        platform::InputEvent event;
        event.source = source;
        event.timestampMillis = nowMillis;
        if (half == "down") {
            event.phase = platform::ButtonPhase::Down;
        } else if (half == "up") {
            event.phase = platform::ButtonPhase::Up;
        } else {
            return unprocessable("'phase' must be 'down' or 'up'");
        }
        context_.input->inject(event);
        return noContent();
    }

    platform::InputEvent down;
    down.source = source;
    down.phase = platform::ButtonPhase::Down;
    down.timestampMillis = nowMillis;
    context_.input->inject(down);

    platform::InputEvent up;
    up.source = source;
    up.phase = platform::ButtonPhase::Up;
    up.timestampMillis = nowMillis + holdMillis;
    context_.input->inject(up);

    return noContent();
}

Response ApiServer::handleSound(const Request& request) {
    platform::IAudioOutput* speaker =
        context_.platform != nullptr ? context_.platform->audio() : nullptr;

    // Absence is reported rather than swallowed (ADR 0013). A route that
    // accepted sounds on a device with no speaker would answer 204 for ever
    // and the caller would have no way to find out why the room was quiet.
    if (speaker == nullptr) {
        return notFound("this device has no speaker");
    }

    if (request.method == Method::Get) {
        // The catalogue, so a caller can offer it rather than hard-code it.
        std::size_t count = 0;
        const audio::Sound* sounds = audio::SoundLibrary::all(count);

        JsonWriter writer;
        writer.beginObject().key("sounds").beginArray();
        for (std::size_t i = 0; i < count; ++i) {
            writer.beginObject()
                .member("name", sounds[i].name)
                .member("durationMillis", sounds[i].durationMillis())
                .endObject();
        }
        writer.endArray();
        // Reported alongside the list because "silent" is a valid setting and
        // not a sound, so a UI building a dropdown needs both facts.
        writer.member("silentName", "none").endObject();
        return ok(writer.take());
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }

    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    // Stopping is its own request rather than a magic name, because "stop" is
    // not a sound and a caller asking for silence should not have to know
    // that the catalogue happens not to contain it.
    if (const json::Value stop = root["stop"]; stop.isBoolean() && stop.toBool(false)) {
        speaker->stop();
        return noContent();
    }

    if (const json::Value name = root["sound"]; name.isString()) {
        const std::string wanted = name.toString();
        if (!speaker->playSound(wanted)) {
            // 422 rather than 404: the route exists and the request was
            // well-formed, the name just is not one this device can make.
            // GET this path to find out which are.
            return unprocessable("'sound' is not a sound this device can play");
        }
        return noContent();
    }

    // An inline tone, for a caller that wants a noise the catalogue does not
    // have. Bounded at both ends: the limits match what a script gets, because
    // there is no reason the network should reach further into the speaker
    // than the device's own code does.
    const json::Value frequency = root["frequencyHz"];
    if (!frequency.isNumber()) {
        return badRequest("'sound', 'frequencyHz' or 'stop' is required");
    }

    const std::int64_t hz = frequency.toInt(0);
    if (hz < kMinToneHz || hz > kMaxToneHz) {
        return unprocessable("'frequencyHz' is out of range");
    }

    std::int64_t millis = kDefaultToneMillis;
    if (const json::Value duration = root["durationMillis"]; duration.isNumber()) {
        millis = duration.toInt(0);
        if (millis <= 0 || millis > kMaxToneMillis) {
            return unprocessable("'durationMillis' is out of range");
        }
    }

    if (!speaker->playTone(static_cast<int>(hz), static_cast<int>(millis))) {
        return unprocessable("the speaker refused the tone");
    }
    return noContent();
}

// --- device information ------------------------------------------------------

Response ApiServer::handleDevice(const Request& request) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }

    JsonWriter writer;
    writer.beginObject();
    writer.member("name", context_.config != nullptr ? context_.config->deviceName : "stipple");
    writer.member("platform",
                  context_.platform != nullptr ? context_.platform->name() : "unknown");
    writer.member("version", kVersion);
    writer.member("apiVersion", kApiVersion);

    // A device nobody has set up yet. The page opens on the network step
    // rather than on a live view of a clock showing the wrong time - not a
    // modal and not a wizard, the same page in a different order (ADR 0018).
    writer.member("firstRun", context_.firstRun != nullptr && *context_.firstRun);

    writer.key("display").beginObject();
    writer.member("width", Framebuffer::kWidth);
    writer.member("height", Framebuffer::kHeight);
    if (context_.platform != nullptr) {
        writer.member("brightness",
                      static_cast<int>(context_.platform->display().brightness()));
        writer.member("minimumFrameIntervalMillis",
                      context_.platform->display().minimumFrameIntervalMillis());
    }
    writer.endObject();

    writer.key("network");
    if (context_.platform != nullptr && context_.platform->network() != nullptr) {
        const platform::NetworkStatus status = context_.platform->network()->status();
        writer.beginObject()
            .member("connected", status.connected)
            .member("ipv4", status.ipv4)
            .member("hostname", status.hostname);
        // Emitted only where it means something. A platform that cannot
        // measure a signal reported a flat zero before, which reads as "no
        // signal" rather than "no measurement" - the same class of lie as a
        // battery at 0% because nothing answered.
        if (status.signalKnown) {
            writer.member("rssiDbm", status.rssiDbm);
        }
        if (!status.ssid.empty()) {
            writer.member("ssid", status.ssid);
        }

        // The lease, where something is managing one. Its absence is the
        // interesting case and is reported as absence: a device running on an
        // address nothing is renewing looks identical to a healthy one right
        // up until the address is taken back.
        writer.member("leaseManaged", status.leaseKnown);
        if (status.leaseKnown) {
            writer.member("leaseState", status.leaseState);
            if (status.leaseSeconds == 0xFFFFFFFFu) {
                writer.member("leaseSeconds", -1);  // granted forever
            } else {
                writer.member("leaseSeconds", static_cast<std::int64_t>(status.leaseSeconds));
            }
        }
        writer.endObject();
    } else {
        // Null rather than a fabricated "disconnected": this platform has no
        // network interface at all, which is different from having one that is
        // down (ADR 0013).
        writer.nullValue();
    }

    // What this build can actually do. A UI that knows the device has no
    // speaker can grey out the volume control instead of offering one that
    // silently does nothing — the same reasoning as ADR 0013, surfaced over
    // HTTP so clients get it too.
    writer.key("capabilities").beginObject();
    if (context_.platform != nullptr) {
        writer.member("audio", context_.platform->audio() != nullptr)
            .member("network", context_.platform->network() != nullptr)
            .member("reboot", context_.platform->rebooter() != nullptr)
            .member("battery", context_.platform->power() != nullptr)
            .member("microphone", context_.platform->microphone() != nullptr)
            // Not "is MQTT available" - that is the mqtt pointer - but "can
            // the transport do TLS if asked". A page that offers the switch
            // without knowing turns a missing feature into a broker that
            // mysteriously stopped answering.
            .member("mqttTls", context_.platform->mqtt() != nullptr &&
                                   context_.platform->mqtt()->supportsTls());
    }
    writer.endObject();

    // Reported separately from the capability flag, because "this device has a
    // battery" and "we currently know its charge" are different facts and a UI
    // needs to tell them apart.
    if (context_.platform != nullptr && context_.platform->power() != nullptr) {
        const platform::BatteryStatus status = context_.platform->power()->battery();
        writer.key("battery").beginObject().member("known", status.known);
        if (status.known) {
            writer.member("percent", status.percent);
            // The number that says whether to believe the percentage. A cell
            // reading 3.15 V is telling you something the percentage alone
            // cannot.
            writer.member("millivolts", status.millivolts);
        }
        // Reported independently of `known`: a platform can know it is on
        // external power without having a charge reading yet, and the flag is
        // what explains a percentage that moves when the cable does.
        if (status.chargingKnown) {
            writer.member("charging", status.charging);
        }
        writer.endObject();
    }

    // Same split as the battery, and for a sharper reason. A microphone that is
    // present but has never delivered a sample is exactly what a TC002 looks
    // like until it is switched on, and reporting only the capability turned
    // that into a visualiser drawing a flat line and calling it silence.
    if (context_.platform != nullptr && context_.platform->microphone() != nullptr) {
        const platform::SoundLevel sound = context_.platform->microphone()->level();
        writer.key("microphone").beginObject().member("known", sound.known);
        if (sound.known) {
            writer.member("amplitude", sound.amplitude);
        }
        writer.endObject();
    }

    writer.endObject();
    return ok(writer.take());
}

Response ApiServer::handleHealth(const Request& request, std::uint64_t nowMillis) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }

    JsonWriter writer;
    writer.beginObject();
    writer.member("status", "ok");

    if (context_.platform != nullptr) {
        writer.member("uptimeMillis",
                      static_cast<std::int64_t>(context_.platform->clock().monotonicMillis()));
        writer.member("wallClockValid", context_.platform->clock().wallClockValid());
    } else {
        writer.member("uptimeMillis", static_cast<std::int64_t>(nowMillis));
    }

    writer.member("apps", context_.apps != nullptr ? context_.apps->count() : 0);
    writer.member("notifications",
                  context_.notifications != nullptr ? context_.notifications->size() : 0);
    writer.endObject();
    return ok(writer.take());
}

Response ApiServer::handleVersion(const Request& request) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }

    JsonWriter writer;
    writer.beginObject()
        .member("version", kVersion)
        .member("commit", kBuildCommit)
        .member("api", kApiVersion)
        .member("schema", config::kCurrentSchemaVersion)
        .member("target", context_.platform != nullptr ? context_.platform->name() : "unknown")
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleDiagnostics(const Request& request, std::uint64_t nowMillis) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }

    JsonWriter writer;
    writer.beginObject();
    writer.member("uptimeMillis", static_cast<std::int64_t>(nowMillis));

    if (context_.apps != nullptr) {
        writer.key("apps").beginObject()
            .member("total", context_.apps->count())
            .member("enabled", context_.apps->enabledCount())
            .endObject();
    }

    if (context_.notifications != nullptr) {
        writer.key("notifications").beginObject()
            .member("active", context_.notifications->active() != nullptr ? 1 : 0)
            .member("pending", context_.notifications->pending())
            .member("dropped",
                    static_cast<std::int64_t>(context_.notifications->droppedCount()))
            .endObject();
    }

    if (context_.platform != nullptr) {
        writer.key("input").beginObject()
            .member("droppedEvents",
                    static_cast<std::int64_t>(context_.platform->input().droppedEventCount()))
            .endObject();
    }

    if (context_.scheduler != nullptr) {
        const render::FrameStats& stats = context_.scheduler->stats();
        writer.key("render").beginObject()
            .member("rendered", static_cast<std::int64_t>(stats.rendered))
            // A healthy static clock face skips far more often than it
            // renders. If this stays at zero, dirty tracking is not working -
            // which is worth being able to see from a browser rather than
            // only from a debugger.
            .member("skipped", static_cast<std::int64_t>(stats.skipped))
            .member("overruns", static_cast<std::int64_t>(stats.overruns))
            .member("lastRenderMillis", static_cast<std::int64_t>(stats.lastRenderMillis))
            .member("worstRenderMillis", static_cast<std::int64_t>(stats.worstRenderMillis))
            .member("intervalMillis", context_.scheduler->intervalMillis())
            .endObject();
    }

    if (context_.carousel != nullptr) {
        const app::App* active = context_.carousel->active();
        writer.key("carousel").beginObject()
            .member("active", active != nullptr ? active->id : std::string())
            .member("paused", context_.carousel->paused())
            .endObject();
    }

    if (context_.glucose != nullptr) {
        // Counts and the last outcome - enough to tell "never configured" from
        // "server refusing us" from "network down" without seeing the URL,
        // which usually names a person, or the credential, which never leaves.
        const apps::glucose::NightscoutSource::Status& status = context_.glucose->status();
        const std::int64_t holdSeconds =
            status.holdUntilMillis > nowMillis
                ? static_cast<std::int64_t>((status.holdUntilMillis - nowMillis) / 1000u)
                : 0;
        const std::int64_t successAge =
            status.lastSuccessMillis > 0
                ? static_cast<std::int64_t>((nowMillis - status.lastSuccessMillis) / 1000u)
                : -1;
        writer.key("glucose").beginObject()
            .member("configured", context_.glucose->configured())
            .member("fetches", static_cast<std::int64_t>(status.fetches))
            .member("failures", static_cast<std::int64_t>(status.failures))
            .member("lastHttpStatus", static_cast<std::int64_t>(status.lastHttpStatus))
            .member("lastBodyBytes", static_cast<std::int64_t>(status.lastBodyBytes))
            .member("lastFailure", std::string(status.lastFailure))
            .member("lastSuccessAgeSeconds", successAge)
            .member("samples", static_cast<std::int64_t>(status.sampleCount))
            .member("fatalStreak", static_cast<std::int64_t>(status.fatalStreak))
            .member("holdSeconds", holdSeconds)
            .member("source", apps::glucose::sourceKindName(context_.glucose->kind()))
            .member("region", std::string(status.region));
        // The reading itself, for the settings page's header. The panel shows
        // it anyway, so this says nothing a person in the room cannot see.
        {
            const apps::glucose::Reading& reading = context_.glucose->reading();
            writer.key("reading").beginObject()
                .member("present", reading.historyCount > 0)
                .member("sgv", reading.sgv)
                .member("trend", apps::glucose::trendName(reading.trendShown()))
                .member("minutesAgo", reading.minutesAgo)
                .member("stale", reading.stale())
                .member("hasDelta", reading.hasDelta)
                .member("delta", reading.delta)
                .endObject();
        }
        // Who a LibreLinkUp account follows, for the settings page's picker.
        // Names the account holder chose to share with this follower login.
        writer.key("patients").beginArray();
        for (int i = 0; i < status.patientCount; ++i) {
            writer.beginObject()
                .member("id", std::string(status.patients[i].id))
                .member("name", std::string(status.patients[i].name))
                .endObject();
        }
        writer.endArray();
        writer.endObject();
    }

    if (context_.glucoseAlarm != nullptr) {
        const apps::glucose::GlucoseAlarm& alarm = *context_.glucoseAlarm;
        const std::uint64_t lastPlay = alarm.lastPlayMillis();
        const char* state = alarm.sounding() ? "sounding" : alarm.snoozed() ? "snoozed" : "quiet";
        writer.key("alarm").beginObject()
            .member("state", state)
            .member("kind", apps::glucose::alarmKindName(alarm.active()))
            .member("snoozedSeconds", alarm.snoozeRemainingSeconds(nowMillis))
            .member("lastPlayAgeSeconds",
                    lastPlay > 0 && nowMillis >= lastPlay
                        ? static_cast<std::int64_t>((nowMillis - lastPlay) / 1000u)
                        : static_cast<std::int64_t>(-1))
            .member("plays", static_cast<std::int64_t>(alarm.plays()))
            .member("playFailures", static_cast<std::int64_t>(alarm.playFailures()))
            .member("melodyFallbacks", static_cast<std::int64_t>(alarm.melodyFallbacks()))
            .member("speaker", context_.platform != nullptr && context_.platform->audio() != nullptr)
            .endObject();
    }

    // Never expose secrets through diagnostics (§22): no tokens, no Wi-Fi
    // credentials, not even a redacted placeholder that confirms one exists.
    writer.endObject();
    return ok(writer.take());
}

Response ApiServer::handleLogs(const Request& request) {
    if (context_.logger == nullptr) {
        return serverError("log unavailable");
    }
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }

    const log::RingLog& logger = *context_.logger;

    JsonWriter writer;
    writer.beginObject().key("entries").beginArray();
    for (int i = 0; i < logger.count(); ++i) {
        const log::RingLog::Entry& entry = logger.at(i);
        writer.beginObject()
            .member("at", static_cast<std::int64_t>(entry.timestampMillis))
            .member("level", log::levelName(entry.level))
            .member("message", entry.message)
            .endObject();
    }
    writer.endArray();

    writer.member("count", logger.count());
    writer.member("capacity", log::RingLog::kCapacity);

    // The ring overwrites, so a reader that only sees `entries` has no way to
    // know history was lost. Reporting the total lets a UI say "24 of 812"
    // instead of implying the device has only ever logged 24 things.
    writer.member("totalWritten", static_cast<std::int64_t>(logger.totalWritten()));
    writer.member("minimumLevel", log::levelName(logger.minimumLevel()));
    writer.endObject();
    return ok(writer.take());
}

// --- apps --------------------------------------------------------------------

namespace {

/// 422 with the validation detail attached.
///
/// The shape is the project's usual error object plus a `warnings` array, so
/// a client already branching on `error.code` keeps working and one that
/// wants to know *which* element was wrong can find out.
Response sceneRefused(const std::vector<std::string>& warnings) {
    JsonWriter writer;
    writer.beginObject().key("error").beginObject()
        .member("code", "unprocessable")
        .member("message", "no element in 'scene' can be drawn")
        .endObject();
    writer.key("warnings").beginArray();
    for (const std::string& warning : warnings) {
        writer.value(warning);
    }
    writer.endArray();
    writer.endObject();
    return Response{422, "application/json", writer.take(), {}, {}, {}};
}

}  // namespace

bool ApiServer::describeScene(std::string_view json, std::vector<std::string>& warnings) {
    scene::Scene scene(sceneTokens_, kSceneTokens);
    if (context_.icons != nullptr) {
        // With the store attached, a scene naming an icon that is not there
        // is reported rather than quietly drawing nothing.
        scene.setIconStore(context_.icons);
    }

    const bool loaded = scene.load(json);

    for (int i = 0; i < scene.issueCount(); ++i) {
        const scene::Issue& issue = scene.issueAt(i);
        std::string detail;
        if (issue.elementIndex >= 0) {
            // Named by element, because "something is wrong" sends whoever
            // wrote the integration through every element by hand.
            detail = "element ";
            detail += std::to_string(issue.elementIndex);
            detail += ": ";
        }
        detail += issue.message;
        warnings.push_back(std::move(detail));
    }
    if (scene.issueOverflow()) {
        warnings.emplace_back("...and more; not all of them were recorded");
    }

    return loaded && scene.anyRenderable();
}

Response ApiServer::handleAppCollection(const Request& request, std::uint64_t nowMillis) {
    if (context_.apps == nullptr) {
        return serverError("app registry unavailable");
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writer.beginObject().key("apps").beginArray();
        for (int i = 0; i < context_.apps->count(); ++i) {
            writeApp(writer, *context_.apps->at(i), i);
        }
        writer.endArray();
        writer.member("count", context_.apps->count());
        writer.endObject();
        return ok(writer.take());
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }

    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    app::App entry;
    entry.id = root["id"].toString();
    if (entry.id.empty()) {
        return unprocessable("'id' is required");
    }
    entry.name = root["name"].toString(entry.id);
    entry.durationSeconds = static_cast<int>(root["durationSeconds"].toInt(0));
    entry.enabled = root["enabled"].toBool(true);
    entry.source = app::AppSource::Remote;

    // Store the scene as the exact text the parser accepted. Because it came
    // out of a successful parse it is valid by construction, which is what lets
    // GET echo it back verbatim without a reserialise round trip.
    const json::Value scene = root["scene"];
    std::vector<std::string> warnings;
    bool renderable = true;
    if (scene.valid()) {
        if (!scene.isObject()) {
            return unprocessable("'scene' must be a JSON object");
        }
        entry.sceneJson = std::string(scene.raw());

        // Checked here rather than taken on trust.
        //
        // This used to store anything that was a JSON object and answer 201.
        // An integration sending elements in a shape the renderer does not
        // accept got a success for every one of them and a black panel, with
        // nothing anywhere saying why.
        renderable = describeScene(entry.sceneJson, warnings);
    }

    if (!renderable) {
        return sceneRefused(warnings);
    }

    if (context_.apps->find(entry.id) != nullptr) {
        return conflict("an app with that id already exists; use PUT to replace it");
    }

    const std::string id = entry.id;
    switch (context_.apps->put(std::move(entry))) {
        case app::AppRegistry::PutResult::Added:
        case app::AppRegistry::PutResult::Replaced:
            break;
        case app::AppRegistry::PutResult::Full:
            return conflict("app registry is full");
        case app::AppRegistry::PutResult::InvalidId:
            return unprocessable("'id' is empty or too long");
        case app::AppRegistry::PutResult::SceneTooLarge:
            return payloadTooLarge("'scene' exceeds the per-app limit");
    }

    if (context_.carousel != nullptr) {
        context_.carousel->tick(nowMillis);
    }

    JsonWriter writer;
    const int position = context_.apps->indexOf(id);
    writeApp(writer, *context_.apps->find(id), position);
    return created(writer.take());
}

Response ApiServer::handleAppItem(const Request& request,
                                  const std::string& id,
                                  std::uint64_t nowMillis) {
    if (context_.apps == nullptr) {
        return serverError("app registry unavailable");
    }

    const app::App* existing = context_.apps->find(id);

    if (request.method == Method::Get) {
        if (existing == nullptr) {
            return notFound("no such app");
        }
        JsonWriter writer;
        writeApp(writer, *existing, context_.apps->indexOf(id));
        return ok(writer.take());
    }

    if (request.method == Method::Delete) {
        if (existing == nullptr) {
            return notFound("no such app");
        }
        if (existing->source == app::AppSource::System) {
            return conflict("system apps cannot be deleted");
        }

        // A script's app is not deletable on its own.
        //
        // Writing a script creates its app, so the invariant is that every
        // script has one. Removing just the app breaks that and leaves the
        // script listed as perfectly fine while having no way to ever reach
        // the panel - and because apps are restored from configuration and
        // scripts from their own blob, the orphan survives a reboot.
        //
        // Deleting the source instead would be worse: somebody tidying their
        // carousel does not expect to lose the code. So this points at the
        // place that does both, and `enabled` remains the way to take a
        // script off the rotation without losing anything.
        if (existing->builtin == app::Builtin::Script) {
            return conflict("this app belongs to a script; delete the script itself, "
                            "or disable the app to take it off the carousel");
        }

        context_.apps->remove(id);
        if (context_.carousel != nullptr) {
            context_.carousel->tick(nowMillis);
        }
        return noContent();
    }

    // PATCH changes only what it names. PUT is a replace and needs the whole
    // entry, which makes it the wrong verb for "turn this app off" - and it is
    // what the config page sends for exactly that.
    if (request.method == Method::Patch) {
        if (existing == nullptr) {
            return notFound("no such app");
        }

        Body patch(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
        if (!patch.valid()) {
            return badRequest(std::string("invalid JSON: ") + patch.errorText());
        }
        const json::Value fields = patch.root();
        if (!fields.isObject()) {
            return badRequest("body must be a JSON object");
        }

        app::App updated = *existing;
        if (const json::Value enabled = fields["enabled"]; enabled.isBoolean()) {
            updated.enabled = enabled.toBool(updated.enabled);
        }
        if (const json::Value name = fields["name"]; name.isString()) {
            updated.name = name.toString();
        }
        if (const json::Value duration = fields["durationSeconds"]; duration.isNumber()) {
            const std::int64_t seconds = duration.toInt(updated.durationSeconds);
            if (seconds < 0 || seconds > 3600) {
                return unprocessable("'durationSeconds' is outside 0-3600");
            }
            updated.durationSeconds = static_cast<int>(seconds);
        }

        // A scene belongs to a replace, not a patch: changing what an app *is*
        // is a different operation from changing whether it is shown.
        if (fields["scene"].valid()) {
            return unprocessable("'scene' cannot be patched; use PUT");
        }

        // Position is a property of the app like any other, so it moves on the
        // same verb. Applied after the put below, because a replace keeps the
        // app where it was and moving it first would move the wrong thing.
        //
        // Named "position" because that is what reading an app calls it. A
        // field a client can read and cannot write back under the same name is
        // a trap, and this one very nearly shipped as "index".
        int moveTo = -1;
        if (const json::Value position = fields["position"]; position.isNumber()) {
            const std::int64_t wanted = position.toInt(-1);
            if (wanted < 0 || wanted >= context_.apps->count()) {
                return unprocessable("'position' is outside the installed apps");
            }
            moveTo = static_cast<int>(wanted);
        }

        switch (context_.apps->put(std::move(updated))) {
            case app::AppRegistry::PutResult::Added:
            case app::AppRegistry::PutResult::Replaced:
                break;
            case app::AppRegistry::PutResult::Full:
                return conflict("app registry is full");
            case app::AppRegistry::PutResult::InvalidId:
                return unprocessable("'id' is empty or too long");
            case app::AppRegistry::PutResult::SceneTooLarge:
                return payloadTooLarge("'scene' exceeds the per-app limit");
        }

        if (moveTo >= 0) {
            context_.apps->move(id, moveTo);
        }

        if (context_.carousel != nullptr) {
            context_.carousel->tick(nowMillis);
        }

        JsonWriter writer;
        writeApp(writer, *context_.apps->find(id), context_.apps->indexOf(id));
        return ok(writer.take());
    }

    if (request.method != Method::Put) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    app::App entry;
    entry.id = id;  // the URL is authoritative; any "id" in the body is ignored
    entry.name = root["name"].toString(existing != nullptr ? existing->name : id);
    entry.durationSeconds = static_cast<int>(
        root["durationSeconds"].toInt(existing != nullptr ? existing->durationSeconds : 0));
    entry.enabled = root["enabled"].toBool(existing == nullptr || existing->enabled);
    entry.source = existing != nullptr ? existing->source : app::AppSource::Remote;
    // Carried over, or a PUT on "clock" would leave a system app whose builtin
    // is None and whose scene is empty - an entry that exists, is enabled, and
    // renders nothing.
    entry.builtin = existing != nullptr ? existing->builtin : app::Builtin::None;

    const json::Value scene = root["scene"];
    std::vector<std::string> warnings;
    if (scene.valid()) {
        if (!scene.isObject()) {
            return unprocessable("'scene' must be a JSON object");
        }
        entry.sceneJson = std::string(scene.raw());

        // Validated on the way in, the same as POST. This is the verb an
        // integration actually uses to refresh a value, so a scene that
        // stopped rendering would otherwise go unnoticed until somebody
        // happened to look at the panel.
        if (!describeScene(entry.sceneJson, warnings)) {
            return sceneRefused(warnings);
        }
    } else if (existing != nullptr) {
        entry.sceneJson = existing->sceneJson;  // omitting 'scene' keeps the current one
    }

    const bool creating = existing == nullptr;
    switch (context_.apps->put(std::move(entry))) {
        case app::AppRegistry::PutResult::Added:
        case app::AppRegistry::PutResult::Replaced:
            break;
        case app::AppRegistry::PutResult::Full:
            return conflict("app registry is full");
        case app::AppRegistry::PutResult::InvalidId:
            return unprocessable("'id' is empty or too long");
        case app::AppRegistry::PutResult::SceneTooLarge:
            return payloadTooLarge("'scene' exceeds the per-app limit");
    }

    if (context_.carousel != nullptr) {
        context_.carousel->tick(nowMillis);
    }

    JsonWriter writer;
    writeApp(writer, *context_.apps->find(id), context_.apps->indexOf(id));
    return creating ? created(writer.take()) : ok(writer.take());
}

Response ApiServer::handleAppActivate(const Request& request,
                                      const std::string& id,
                                      std::uint64_t nowMillis) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.apps == nullptr || context_.carousel == nullptr) {
        return serverError("carousel unavailable");
    }

    const app::App* entry = context_.apps->find(id);
    if (entry == nullptr) {
        return notFound("no such app");
    }
    if (!entry->enabled) {
        return conflict("app is disabled");
    }

    if (!context_.carousel->activate(id, nowMillis)) {
        return conflict("app could not be activated");
    }

    JsonWriter writer;
    writer.beginObject()
        .member("activeApp", id)
        .member("pinned", context_.carousel->isPinned())
        .endObject();
    return ok(writer.take());
}

// --- notifications -----------------------------------------------------------

Response ApiServer::handleNotificationCollection(const Request& request,
                                                 std::uint64_t nowMillis) {
    if (context_.notifications == nullptr) {
        return serverError("notification queue unavailable");
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writer.beginObject();
        writer.key("active");
        if (const notify::Notification* active = context_.notifications->active()) {
            writeNotification(writer, *active);
        } else {
            writer.nullValue();
        }
        writer.member("pending", context_.notifications->pending());
        writer.member("dropped",
                      static_cast<std::int64_t>(context_.notifications->droppedCount()));
        writer.endObject();
        return ok(writer.take());
    }

    if (request.method == Method::Delete) {
        const int removed = context_.notifications->dismissAll(nowMillis);
        JsonWriter writer;
        writer.beginObject().member("dismissed", removed).endObject();
        return ok(writer.take());
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    notify::Notification notification;
    notification.id = root["id"].toString();
    notification.text = root["text"].toString();
    notification.priority =
        notify::priorityFromInt(static_cast<int>(root["priority"].toInt(1)));
    notification.durationSeconds = static_cast<int>(root["durationSeconds"].toInt(5));
    notification.hold = root["hold"].toBool(false);
    notification.dismissible = root["dismissible"].toBool(true);
    notification.sound = root["sound"].toString();

    // Named rather than uploaded alongside: an icon is reusable and a
    // notification is not, so the same glyph should not arrive again with
    // every alert on a device with this little RAM. An id that is not stored
    // renders as text alone rather than failing the request - the message is
    // the point, and refusing to show it because a decoration is missing
    // would be the worst possible trade.
    notification.icon = root["icon"].toString();

    if (notification.text.empty()) {
        return unprocessable("'text' is required");
    }

    switch (context_.notifications->push(std::move(notification), nowMillis)) {
        case notify::NotificationQueue::PushResult::Invalid:
            return unprocessable("notification text or id exceeds its limit");
        case notify::NotificationQueue::PushResult::DroppedLowPriority:
            // 429 is the honest answer: the request was well-formed, the device
            // is simply saturated and said so rather than pretending.
            return error(429, "queue_full",
                         "notification queue is full and nothing queued ranks lower");
        default:
            break;
    }

    JsonWriter writer;
    writer.beginObject();
    writer.key("active");
    if (const notify::Notification* active = context_.notifications->active()) {
        writeNotification(writer, *active);
    } else {
        writer.nullValue();
    }
    writer.member("pending", context_.notifications->pending());
    writer.endObject();
    return created(writer.take());
}

Response ApiServer::handleNotificationItem(const Request& request,
                                           const std::string& id,
                                           std::uint64_t nowMillis) {
    if (context_.notifications == nullptr) {
        return serverError("notification queue unavailable");
    }
    if (request.method != Method::Delete) {
        return methodNotAllowed();
    }

    if (!context_.notifications->dismiss(id, nowMillis)) {
        // Cannot distinguish "absent" from "refuses to be dismissed" without
        // leaking which ids exist, so both answer 404 and the queue's own
        // semantics are documented instead.
        return notFound("no such notification, or it cannot be dismissed");
    }
    return noContent();
}

// --- assets ------------------------------------------------------------------

namespace {

/// `withPixels` is only ever set for a single-icon fetch.
///
/// The collection deliberately stays metadata: a store holding sixty-four
/// icons could otherwise answer one request with several hundred kilobytes
/// of JSON, on a device that has to build the whole string in RAM before it
/// can send any of it.
void writeIcon(JsonWriter& writer, const asset::Icon& icon, bool withPixels = false) {
    writer.beginObject()
        .member("id", icon.id)
        .member("width", icon.width)
        .member("height", icon.height)
        .member("frames", icon.frameCount)
        .member("frameMillis", static_cast<std::int64_t>(icon.frameMillis))
        .member("bytes", static_cast<std::int64_t>(icon.byteSize()));
    writer.key("transparent");
    if (icon.hasTransparency) {
        writer.value(static_cast<std::int64_t>(toPacked(icon.transparent)));
    } else {
        writer.nullValue();
    }

    if (withPixels) {
        // Named `pixels` rather than `frames`, because `frames` already means
        // the count here and changing it would break every existing reader.
        // The shape matches what POST accepts, so an icon fetched from one
        // device can be posted to another without translation.
        const std::size_t perFrame = icon.pixelsPerFrame();
        writer.key("pixels").beginArray();
        for (int frame = 0; frame < icon.frameCount; ++frame) {
            writer.beginArray();
            const std::size_t base = static_cast<std::size_t>(frame) * perFrame;
            for (std::size_t i = 0; i < perFrame; ++i) {
                writer.value(static_cast<std::int64_t>(toPacked(icon.pixels[base + i])));
            }
            writer.endArray();
        }
        writer.endArray();
    }

    writer.endObject();
}

}  // namespace

Response ApiServer::handleAssetCollection(const Request& request) {
    if (context_.icons == nullptr) {
        return serverError("icon store unavailable");
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writer.beginObject().key("assets").beginArray();
        for (int i = 0; i < context_.icons->count(); ++i) {
            writeIcon(writer, *context_.icons->at(i));
        }
        writer.endArray();
        writer.member("count", context_.icons->count());
        // Storage pressure is worth surfacing: an upload that fails because the
        // budget is full should be predictable, not a surprise.
        writer.member("bytesUsed", static_cast<std::int64_t>(context_.icons->bytesUsed()));
        writer.member("bytesFree", static_cast<std::int64_t>(context_.icons->bytesFree()));
        writer.endObject();
        return ok(writer.take());
    }

    if (request.method == Method::Delete) {
        context_.icons->clear();
        return noContent();
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    // Every pixel is a JSON token, so an icon needs a budget an ordinary
    // request does not. A 16x16 frame alone is 256 of them.
    Body body(request.body, options_.maxIconJsonTokens, options_.maxIconBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    asset::Icon icon;
    icon.id = root["id"].toString();
    icon.width = static_cast<int>(root["width"].toInt(0));
    icon.height = static_cast<int>(root["height"].toInt(0));
    icon.frameMillis = static_cast<std::uint32_t>(root["frameMillis"].toInt(100));

    // Packed 0xRRGGBB only. The converter that produces these is code, not a
    // person, so the several human-friendly colour forms the scene model accepts
    // would be surface for nothing.
    if (const json::Value keyed = root["transparent"]; keyed.isNumber()) {
        const std::int64_t packed = keyed.toInt(-1);
        if (packed < 0 || packed > 0xFFFFFF) {
            return unprocessable("'transparent' must be a packed 0xRRGGBB value");
        }
        icon.hasTransparency = true;
        icon.transparent = fromPacked(static_cast<std::uint32_t>(packed));
    }

    const json::Value frames = root["frames"];
    if (!frames.isArray() || frames.size() == 0) {
        return unprocessable("'frames' must be a non-empty array of pixel arrays");
    }
    icon.frameCount = frames.size();

    // Reject the geometry before reserving anything, so an absurd declared size
    // cannot make us allocate first and fail second.
    if (icon.width <= 0 || icon.height <= 0 ||
        icon.width > asset::IconStore::kMaxDimension ||
        icon.height > asset::IconStore::kMaxDimension ||
        icon.frameCount > asset::IconStore::kMaxFrames) {
        return unprocessable("width, height or frame count is out of range");
    }

    const std::size_t perFrame = icon.pixelsPerFrame();
    icon.pixels.reserve(perFrame * static_cast<std::size_t>(icon.frameCount));

    for (int f = 0; f < icon.frameCount; ++f) {
        const json::Value frame = frames[f];
        if (!frame.isArray() || frame.size() != static_cast<int>(perFrame)) {
            return unprocessable("each frame must hold exactly width x height pixels");
        }
        for (int i = 0; i < frame.size(); ++i) {
            const std::int64_t packed = frame[i].toInt(-1);
            if (packed < 0 || packed > 0xFFFFFF) {
                return unprocessable("pixels must be packed 0xRRGGBB values");
            }
            icon.pixels.push_back(fromPacked(static_cast<std::uint32_t>(packed)));
        }
    }

    const std::string id = icon.id;
    const asset::IconStore::PutResult result = context_.icons->put(std::move(icon));
    switch (result) {
        case asset::IconStore::PutResult::Added:
        case asset::IconStore::PutResult::Replaced:
            break;
        case asset::IconStore::PutResult::InvalidId:
        case asset::IconStore::PutResult::InvalidGeometry:
            return unprocessable(asset::IconStore::describe(result));
        case asset::IconStore::PutResult::TooManyIcons:
        case asset::IconStore::PutResult::BudgetExceeded:
            return conflict(asset::IconStore::describe(result));
    }

    JsonWriter writer;
    writeIcon(writer, *context_.icons->find(id));
    return result == asset::IconStore::PutResult::Added ? created(writer.take())
                                                        : ok(writer.take());
}

Response ApiServer::handleAssetItem(const Request& request, const std::string& id) {
    if (context_.icons == nullptr) {
        return serverError("icon store unavailable");
    }

    const asset::Icon* icon = context_.icons->find(id);

    if (request.method == Method::Get) {
        if (icon == nullptr) {
            return notFound("no such icon");
        }
        JsonWriter writer;
        writeIcon(writer, *icon, /*withPixels=*/true);
        return ok(writer.take());
    }

    if (request.method != Method::Delete) {
        return methodNotAllowed();
    }
    if (!context_.icons->remove(id)) {
        return notFound("no such icon");
    }
    return noContent();
}

// --- scripts -----------------------------------------------------------------

namespace {

/// A script, as JSON.
///
/// `source` is omitted from the collection on purpose. Sixteen scripts at up
/// to 16 KB each would make a list request answer with a quarter of a megabyte
/// on a device with 36 MB of RAM, and the list is what the web UI asks for
/// every time somebody opens the scripts panel. The editor fetches one script
/// at a time, which is also the only time anyone needs the text.
void writeScript(JsonWriter& writer, const script::Script& entry, bool withSource) {
    writer.beginObject();
    writer.member("id", entry.id);
    writer.member("name", entry.name);
    writer.member("ok", entry.ok);

    // Always present, empty when fine. A caller should not have to tell the
    // difference between "no problem" and "the field is missing because this
    // firmware does not report problems".
    writer.member("problem", entry.problem);

    writer.member("bytes", static_cast<std::int64_t>(entry.source.size()));
    writer.member("lastInstructions", static_cast<std::int64_t>(entry.lastInstructions));
    writer.member("memoryBytes", static_cast<std::int64_t>(entry.memoryBytes));
    if (withSource) {
        writer.member("source", entry.source);
    }
    writer.endObject();
}

/// The `@config` fields a script declared, with what each currently holds.
///
/// Only on the single-script fetch. The list view is metadata, and the editor
/// is the only place anybody fills a form in.
void writeSettings(JsonWriter& writer, const script::IScriptRunner& scripts,
                   const std::string& id) {
    writer.key("settings").beginArray();
    for (const script::Setting& setting : scripts.settings(id)) {
        writer.beginObject()
            .member("key", setting.key)
            .member("type", script::settingTypeName(setting.type))
            .member("label", setting.label)
            .member("help", setting.help)
            .member("default", setting.fallback);

        // Empty means nothing has been set and the script's own fallback
        // applies. Deliberately distinct from a stored empty string, which
        // is a value somebody chose.
        writer.member("value", scripts.settingValue(id, setting.key));

        if (setting.type == script::Setting::Type::Text && setting.maxLength > 0) {
            writer.member("maxLength", static_cast<std::int64_t>(setting.maxLength));
        }
        if (setting.type == script::Setting::Type::Number && setting.bounded) {
            writer.member("minimum", static_cast<std::int64_t>(setting.minimum));
            writer.member("maximum", static_cast<std::int64_t>(setting.maximum));
        }
        writer.endObject();
    }
    writer.endArray();
}

Response noScripting() {
    // Not a 500. The device is working exactly as built; it simply has no
    // interpreter in it, and saying "internal error" would send somebody
    // looking for a fault that is not there.
    return error(501, "unsupported", "this firmware was built without scripting");
}

}  // namespace

Response ApiServer::handleScriptCollection(const Request& request) {
    if (context_.scripts == nullptr) {
        return noScripting();
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writer.beginObject().key("scripts").beginArray();
        for (int i = 0; i < context_.scripts->count(); ++i) {
            writeScript(writer, *context_.scripts->at(i), /*withSource=*/false);
        }
        writer.endArray();
        writer.member("count", context_.scripts->count());
        writer.member("capacity", context_.scripts->capacity());
        // So the editor can refuse an over-long paste before it is sent,
        // rather than after the author has lost it to a 422.
        writer.member("maxSourceBytes",
                      static_cast<std::int64_t>(context_.scripts->maxSourceBytes()));
        writer.member("memoryBytes",
                      static_cast<std::int64_t>(context_.scripts->memoryBytes()));
        writer.endObject();
        return ok(writer.take());
    }

    if (request.method == Method::Delete) {
        context_.scripts->clear();
        return noContent();
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    const std::string id = root["id"].toString();
    std::string name = root["name"].toString();
    if (name.empty()) {
        name = id;
    }
    const json::Value source = root["source"];
    if (!source.isString()) {
        return unprocessable("'source' must be a string");
    }

    // Asked before the put, because ScriptPutResult collapses added and
    // replaced into DidNotCompile when the source is broken - and a script
    // that did not exist a moment ago was created, whether or not it
    // compiles. Reading the status off the enum alone answered 200 for a new
    // resource, which is a lie about what just happened.
    const bool existed = context_.scripts->find(id) != nullptr;

    const script::ScriptPutResult result =
        context_.scripts->put(id, std::move(name), source.toString());

    switch (result) {
        case script::ScriptPutResult::Added:
        case script::ScriptPutResult::Replaced:
        case script::ScriptPutResult::DidNotCompile:
            // DidNotCompile is a success. The source is stored, and the
            // response carries `ok:false` and the compiler's message so the
            // editor can put a marker on the line that caused it. Failing the
            // request would mean the device refuses to hold work in progress,
            // which is most of what an editor holds.
            break;
        case script::ScriptPutResult::InvalidId:
        case script::ScriptPutResult::SourceTooLarge:
            return unprocessable(script::describeScriptPut(result));
        case script::ScriptPutResult::TooManyScripts:
            return conflict(script::describeScriptPut(result));
    }

    const script::Script* stored = context_.scripts->find(id);
    if (stored == nullptr) {
        return serverError("the script was accepted but cannot be read back");
    }

    // A script is an app. Writing one puts it in the carousel.
    //
    // Done here rather than left to the caller because otherwise every client
    // would have to know the convention, and one that did not would leave the
    // author with a saved script that never appears on the panel and no
    // indication why. Two calls that must always be made together are better
    // made as one.
    //
    // Only when the app is absent. A replacement would reset the position,
    // the duration and the enabled switch, so editing a script would silently
    // undo the carousel arrangement around it.
    if (context_.apps != nullptr && context_.apps->find(id) == nullptr) {
        app::App entry;
        entry.id = id;
        entry.name = stored->name;
        entry.builtin = app::Builtin::Script;
        entry.source = app::AppSource::Local;

        // A script with a duration() gets what it asked for, rounded to whole
        // seconds because that is what the carousel deals in. Only on the
        // first save: after that it is the user's setting, and a script
        // overwriting it on every edit would undo a choice they made on
        // purpose. The script declares a default, not a policy.
        const std::uint32_t wanted = context_.scripts->durationMillis(id);
        if (wanted > 0) {
            entry.durationSeconds = static_cast<int>((wanted + 999u) / 1000u);
        }

        context_.apps->put(std::move(entry));
    }

    JsonWriter writer;
    writeScript(writer, *stored, /*withSource=*/false);
    return existed ? ok(writer.take()) : created(writer.take());
}

Response ApiServer::handleScriptItem(const Request& request, const std::string& id) {
    if (context_.scripts == nullptr) {
        return noScripting();
    }

    if (request.method == Method::Get) {
        const script::Script* entry = context_.scripts->find(id);
        if (entry == nullptr) {
            return notFound("no such script");
        }
        // Built by hand rather than through writeScript, because the
        // settings belong inside the same object and writeScript closes it.
        JsonWriter writer;
        writer.beginObject();
        writer.member("id", entry->id);
        writer.member("name", entry->name);
        writer.member("ok", entry->ok);
        writer.member("problem", entry->problem);
        writer.member("bytes", static_cast<std::int64_t>(entry->source.size()));
        writer.member("lastInstructions",
                      static_cast<std::int64_t>(entry->lastInstructions));
        writer.member("memoryBytes", static_cast<std::int64_t>(entry->memoryBytes));
        writer.member("source", entry->source);
        writeSettings(writer, *context_.scripts, id);
        writer.endObject();
        return ok(writer.take());
    }

    // Settings only. The source is written by POSTing to the collection,
    // which is a different operation with different consequences - saving a
    // channel ID should not be able to recompile anything.
    if (request.method == Method::Patch) {
        if (context_.scripts->find(id) == nullptr) {
            return notFound("no such script");
        }

        Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
        if (!body.valid()) {
            return badRequest(std::string("invalid JSON: ") + body.errorText());
        }
        const json::Value root = body.root();
        if (!root.isObject()) {
            return badRequest("body must be a JSON object");
        }
        const json::Value wanted = root["settings"];
        if (!wanted.isObject()) {
            return unprocessable("expected a \"settings\" object");
        }

        // Every field or none. A partial apply leaves the device in a state
        // the person who sent it did not ask for and cannot see.
        std::string refused;
        for (int i = 0; i < wanted.size(); ++i) {
            const json::Value key = wanted.keyAt(i);
            const json::Value value = wanted.valueAt(i);
            if (!key.isString()) {
                continue;
            }
            std::string text;
            if (value.isString()) {
                text = value.toString();
            } else if (value.isNumber()) {
                text = std::to_string(value.toInt(0));
            } else if (value.isBoolean()) {
                text = value.toBool(false) ? "true" : "false";
            } else {
                refused = key.toString();
                break;
            }
            if (!context_.scripts->setSetting(id, key.toString(), text)) {
                refused = key.toString();
                break;
            }
        }
        if (!refused.empty()) {
            return unprocessable("cannot set \"" + refused +
                                 "\": no such setting, or the value does not fit it");
        }

        JsonWriter writer;
        writer.beginObject();
        writer.member("id", id);
        writeSettings(writer, *context_.scripts, id);
        writer.endObject();
        return ok(writer.take());
    }

    if (request.method != Method::Delete) {
        return methodNotAllowed();
    }
    if (!context_.scripts->remove(id)) {
        return notFound("no such script");
    }

    // And its app goes with it, for the same reason it arrived with it. An app
    // left behind would show SCRIPT ? for ever, which is honest but is not
    // what anybody deleting a script meant to happen.
    if (context_.apps != nullptr) {
        const app::App* entry = context_.apps->find(id);
        if (entry != nullptr && entry->builtin == app::Builtin::Script) {
            context_.apps->remove(id);
        }
    }
    return noContent();
}

// --- settings ----------------------------------------------------------------

Response ApiServer::handleSettings(const Request& request) {
    if (context_.config == nullptr) {
        return serverError("configuration unavailable");
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writeSettings(writer, *context_.config);
        return ok(writer.take());
    }

    if (request.method != Method::Patch) {
        return methodNotAllowed();
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    // PATCH: absent fields keep their current value. Applied to a copy so a
    // rejected value cannot leave settings half-updated.
    config::Config updated = *context_.config;

    if (const json::Value name = root["deviceName"]; name.isString()) {
        const std::string text = name.toString();
        if (text.empty() || text.size() > 64) {
            return unprocessable("'deviceName' must be 1-64 characters");
        }
        updated.deviceName = text;
    }

    if (const json::Value display = root["display"]; display.isObject()) {
        if (const json::Value brightness = display["brightness"]; brightness.isNumber()) {
            const std::int64_t value = brightness.toInt(-1);
            if (value < 0 || value > 255) {
                return unprocessable("'display.brightness' must be 0-255");
            }
            updated.display.brightness = static_cast<std::uint8_t>(value);
        }
        if (const json::Value power = display["power"]; power.isBoolean()) {
            updated.display.power = power.toBool(true);
        }
        if (const json::Value overlay = display["overlay"]; overlay.isString()) {
            // Round-tripped, like clock.theme. Falling back silently would
            // leave a client believing it had selected weather it had not.
            const std::string name = overlay.toString();
            if (render::overlayName(render::overlayFromName(name)) != name) {
                return unprocessable("'display.overlay' is not a known overlay");
            }
            updated.display.overlay = name;
        }
        if (const json::Value night = display["night"]; night.isObject()) {
            if (const json::Value enabled = night["enabled"]; enabled.isBoolean()) {
                updated.display.night.enabled = enabled.toBool(false);
            }
            // A time of day cannot be outside a day. Refused rather than
            // clamped: a caller that sent 1500 meant something, and quietly
            // turning it into 23:59 would be answering a question it did not
            // ask.
            const auto readMinutes = [&night](const char* key, int& into) {
                const json::Value value = night[key];
                if (!value.isNumber()) {
                    return true;
                }
                const std::int64_t minutes = value.toInt(-1);
                if (minutes < 0 || minutes > 1439) {
                    return false;
                }
                into = static_cast<int>(minutes);
                return true;
            };
            if (!readMinutes("startMinutes", updated.display.night.startMinutes)) {
                return unprocessable("'display.night.startMinutes' must be 0-1439");
            }
            if (!readMinutes("endMinutes", updated.display.night.endMinutes)) {
                return unprocessable("'display.night.endMinutes' must be 0-1439");
            }
            if (const json::Value level = night["brightness"]; level.isNumber()) {
                const std::int64_t value = level.toInt(-1);
                if (value < 0 || value > 255) {
                    return unprocessable("'display.night.brightness' must be 0-255");
                }
                updated.display.night.brightness = static_cast<std::uint8_t>(value);
            }
        }
    }

    if (const json::Value audio = root["audio"]; audio.isObject()) {
        if (const json::Value volume = audio["volumePercent"]; volume.isNumber()) {
            const std::int64_t value = volume.toInt(-1);
            if (value < 0 || value > 100) {
                return unprocessable("'audio.volumePercent' must be 0-100");
            }
            updated.audio.volumePercent = static_cast<std::uint8_t>(value);
        }
    }

    if (const json::Value mqtt = root["mqtt"]; mqtt.isObject()) {
        if (const json::Value value = mqtt["enabled"]; value.isBoolean()) {
            updated.mqtt.enabled = value.toBool(false);
        }
        if (const json::Value value = mqtt["host"]; value.isString()) {
            const std::string host = value.toString();
            if (host.size() > 255) {
                return unprocessable("'mqtt.host' is too long");
            }
            updated.mqtt.host = host;
        }
        if (const json::Value value = mqtt["port"]; value.isNumber()) {
            const std::int64_t port = value.toInt(-1);
            if (port < 1 || port > 65535) {
                return unprocessable("'mqtt.port' must be 1-65535");
            }
            updated.mqtt.port = static_cast<int>(port);
        }
        if (const json::Value value = mqtt["clientId"]; value.isString()) {
            updated.mqtt.clientId = value.toString();
        }
        if (const json::Value value = mqtt["baseTopic"]; value.isString()) {
            const std::string topic = value.toString();
            // Wildcards in a base topic would make this device publish to a
            // filter, which no broker will accept and which is confusing to
            // diagnose from the other end.
            if (topic.empty() || topic.find('#') != std::string::npos ||
                topic.find('+') != std::string::npos) {
                return unprocessable("'mqtt.baseTopic' must be non-empty and contain no wildcards");
            }
            updated.mqtt.baseTopic = topic;
        }
        if (const json::Value value = mqtt["username"]; value.isString()) {
            updated.mqtt.username = value.toString();
        }
        // Write-only: accepted, never returned. An empty string clears it, which
        // is the only way to remove a stored credential through the API.
        if (const json::Value value = mqtt["password"]; value.isString()) {
            updated.mqtt.password = value.toString();
        }
        if (const json::Value value = mqtt["tls"]; value.isBoolean()) {
            updated.mqtt.tls = value.toBool(false);
        }
        if (const json::Value value = mqtt["keepAliveSeconds"]; value.isNumber()) {
            const std::int64_t seconds = value.toInt(-1);
            if (seconds < 5 || seconds > 65535) {
                return unprocessable("'mqtt.keepAliveSeconds' must be 5-65535");
            }
            updated.mqtt.keepAliveSeconds = static_cast<int>(seconds);
        }
        if (const json::Value value = mqtt["discovery"]; value.isBoolean()) {
            updated.mqtt.discovery = value.toBool(false);
        }
    }

    if (const json::Value web = root["web"]; web.isObject()) {
        const json::Value userValue = web["username"];
        const json::Value passValue = web["password"];

        if (userValue.isString()) {
            const std::string user = userValue.toString();
            if (user.size() > 64) {
                return unprocessable("'web.username' is at most 64 characters");
            }
            // A colon cannot appear in a Basic username: the credential is
            // "user:password" and the first colon is the separator, so a
            // username containing one could never be sent back. Refused with
            // a reason rather than accepted and then permanently unusable -
            // which on an authentication setting means locked out.
            if (user.find(':') != std::string::npos) {
                return unprocessable("'web.username' cannot contain a colon");
            }
            updated.web.username = user;
        }

        // Write-only: accepted, never returned. An empty string clears it,
        // which is the only way to remove a stored credential through the
        // API.
        if (passValue.isString()) {
            const std::string password = passValue.toString();
            if (password.size() > 128) {
                return unprocessable("'web.password' is at most 128 characters");
            }
            updated.web.password = password;
        }

        // Turning it on needs both. Half-configured is refused here rather
        // than half-applied, because the failure mode of getting this wrong
        // is a device nobody can log into - and unlike most settings, the
        // page that would fix it is behind the thing that broke.
        if (!updated.web.username.empty() && updated.web.password.empty()) {
            return unprocessable(
                "'web.password' is required when 'web.username' is set");
        }
    }

    if (const json::Value apps = root["apps"]; apps.isObject()) {
        if (const json::Value duration = apps["defaultDurationSeconds"]; duration.isNumber()) {
            const std::int64_t value = duration.toInt(-1);
            if (value < 1 || value > 3600) {
                return unprocessable("'apps.defaultDurationSeconds' must be 1-3600");
            }
            updated.apps.defaultDurationSeconds = static_cast<int>(value);
        }
        if (const json::Value transitions = apps["transitions"]; transitions.isBoolean()) {
            updated.apps.transitions = transitions.toBool(true);
        }
        if (const json::Value autoAdvance = apps["autoAdvance"]; autoAdvance.isBoolean()) {
            updated.apps.autoAdvance = autoAdvance.toBool(true);
        }
        if (const json::Value order = apps["order"]; order.isArray()) {
            // Replaced wholesale rather than merged. An order is a sequence,
            // and merging two sequences has no meaning that a caller could
            // predict.
            std::vector<config::AppPreference> wanted;
            const int count = order.size();
            if (count > config::kMaxRememberedApps) {
                return unprocessable("'apps.order' lists more apps than this device can hold");
            }
            for (int i = 0; i < count; ++i) {
                const json::Value entry = order[i];
                if (!entry.isObject()) {
                    return unprocessable("'apps.order' entries must be objects");
                }
                config::AppPreference preference;
                preference.id = entry["id"].toString(std::string());
                if (preference.id.empty()) {
                    return unprocessable("'apps.order' entries need an 'id'");
                }
                preference.enabled = entry["enabled"].toBool(true);
                const std::int64_t seconds = entry["durationSeconds"].toInt(0);
                if (seconds < 0 || seconds > 3600) {
                    return unprocessable("'apps.order' durationSeconds is outside 0-3600");
                }
                preference.durationSeconds = static_cast<int>(seconds);
                wanted.push_back(std::move(preference));
            }
            updated.apps.order = std::move(wanted);
        }
        if (const json::Value style = apps["transition"]; style.isString()) {
            // Only names that round-trip, like the clock face and the
            // visualiser style. Falling back silently would leave a client
            // believing it had chosen an animation it had not.
            const std::string name = style.toString();
            if (render::transitionStyleName(render::transitionStyleFromName(name)) != name) {
                return unprocessable("'apps.transition' is not a known transition");
            }
            updated.apps.transition = name;
        }
    }

    if (const json::Value notifications = root["notifications"]; notifications.isObject()) {
        if (const json::Value sound = notifications["sound"]; sound.isString()) {
            updated.notifications.sound = sound.toString();
        }
    }

    if (const json::Value visualizer = root["visualizer"]; visualizer.isObject()) {
        if (const json::Value style = visualizer["style"]; style.isString()) {
            // Only accept names that round-trip, for the same reason the clock
            // face does: falling back silently would leave a client believing
            // it had selected something it had not.
            const std::string name = style.toString();
            if (apps::visualizerStyleName(apps::visualizerStyleFromName(name)) != name) {
                return unprocessable("'visualizer.style' is not a known style");
            }
            updated.visualizer.style = name;
        }
    }

    if (const json::Value glucose = root["glucose"]; glucose.isObject()) {
        if (const json::Value value = glucose["url"]; value.isString()) {
            std::string url = value.toString();
            while (!url.empty() && url.back() == '/') {
                url.pop_back();
            }
            // Validated as the URL that will actually be fetched, so a base
            // that only fails once the path is appended is refused here rather
            // than discovered as a fetch that never works.
            if (!url.empty()) {
                if (url.size() > apps::glucose::NightscoutSource::kMaxBaseUrlBytes) {
                    return unprocessable("'glucose.url' is too long");
                }
                net::http::Url parsed;
                if (!net::http::parseUrl(apps::glucose::NightscoutSource::entriesUrl(url), parsed)) {
                    return unprocessable("'glucose.url' must be an http:// or https:// URL");
                }
            }
            updated.glucose.url = url;
        }
        // Write-only, and never kept as typed: Nightscout checks the SHA-1 of
        // the secret, so the SHA-1 is the only form the device stores. An empty
        // string clears it, as with every other credential here.
        if (const json::Value value = glucose["apiSecret"]; value.isString()) {
            const std::string secret = value.toString();
            if (secret.size() > 128) {
                return unprocessable("'glucose.apiSecret' is too long");
            }
            updated.glucose.apiSecretSha1 =
                secret.empty() ? std::string() : Sha1::hex(secret.data(), secret.size());
        }
        if (const json::Value value = glucose["pollSeconds"]; value.isNumber()) {
            const std::int64_t seconds = value.toInt(-1);
            if (seconds < apps::glucose::NightscoutSource::kMinPollSeconds ||
                seconds > apps::glucose::NightscoutSource::kMaxPollSeconds) {
                return unprocessable("'glucose.pollSeconds' must be 30-600");
            }
            updated.glucose.pollSeconds = static_cast<int>(seconds);
        }
        {
            std::string error;
            if (!config::applyGlucoseSourceSettings(glucose, updated.glucose, error)) {
                return unprocessable(error);
            }
        }
        // Face, faces, cycling and schedule are one block with cross-field
        // rules (the default must be in use, cycling needs two faces...), so
        // they are applied and checked together.
        {
            std::string error;
            if (!config::applyGlucoseFaceSettings(glucose, updated.glucose, error)) {
                return unprocessable(error);
            }
        }
        if (const json::Value value = glucose["pinned"]; value.valid()) {
            if (!value.isBoolean()) {
                return unprocessable("'glucose.pinned' must be true or false");
            }
            updated.glucose.pinned = value.toBool(true);
        }
        // The alarm block is validated whole, strictly, onto the copy: one
        // wrong field refuses the request and changes nothing.
        if (const json::Value value = glucose["alarms"]; value.valid()) {
            std::string error;
            if (!config::applyAlarmSettings(value, updated.glucose.alarms, error)) {
                return unprocessable(error);
            }
        }
    }

    if (const json::Value clock = root["clock"]; clock.isObject()) {
        if (const json::Value twentyFour = clock["twentyFourHour"]; twentyFour.isBoolean()) {
            updated.clock.twentyFourHour = twentyFour.toBool(true);
        }
        if (const json::Value zone = clock["timezone"]; zone.isString()) {
            // Validated here rather than discovered at render time. An empty
            // string is the documented way to say "use the fixed offset", so
            // it is accepted; anything else has to be a rule this device can
            // actually follow, or the clock would be quietly wrong for half
            // the year with nothing to show for it.
            const std::string spec = zone.toString();
            stipple::timezone_::Timezone parsed;
            if (!spec.empty() && !stipple::timezone_::Timezone::parse(spec, parsed)) {
                return unprocessable("'clock.timezone' is not a POSIX timezone rule");
            }
            updated.clock.timezone = spec;
        }
        if (const json::Value server = clock["ntpServer"]; server.isString()) {
            // Bounded and sanity-checked, not trusted. This string is handed
            // to a resolver, and an unbounded one from the network is how a
            // config field becomes a memory problem.
            const std::string spec = server.toString();
            if (spec.size() > 253) {
                return unprocessable("'clock.ntpServer' is too long to be a hostname");
            }
            for (const char c : spec) {
                const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
                if (!allowed) {
                    return unprocessable("'clock.ntpServer' must be a hostname or address");
                }
            }
            // Empty is meaningful: it turns synchronisation off, for a
            // network that blocks NTP or a user who would rather it did not
            // talk to anyone.
            updated.clock.ntpServer = spec;
        }
        if (const json::Value tick = clock["tick"]; tick.isBoolean()) {
            updated.clock.tick = tick.toBool(false);
        }
        if (const json::Value theme = clock["theme"]; theme.isString()) {
            // Only accept names that round-trip. Falling back silently would
            // leave a client believing it had selected a face it had not.
            const std::string name = theme.toString();
            if (apps::clockThemeName(apps::clockThemeFromName(name)) != name) {
                return unprocessable("'clock.theme' is not a known clock face");
            }
            updated.clock.theme = name;
        }
        if (const json::Value offset = clock["utcOffsetSeconds"]; offset.isNumber()) {
            const std::int64_t value = offset.toInt(0);
            if (value < -12 * 3600 || value > 14 * 3600) {
                return unprocessable("'clock.utcOffsetSeconds' is outside any real time zone");
            }
            updated.clock.utcOffsetSeconds = static_cast<int>(value);
        }
        if (const json::Value value = clock["leadingZero"]; value.isBoolean()) {
            updated.clock.leadingZero = value.toBool(true);
        }
        if (const json::Value value = clock["showAmPm"]; value.isBoolean()) {
            updated.clock.showAmPm = value.toBool(false);
        }

        // Same contract as `theme`: only names that round-trip are accepted, so
        // a client is never left believing it selected something it did not.
        struct NameField {
            const char* key;
            std::string* target;
            std::string (*canonical)(const std::string&);
            const char* complaint;
        };
        const NameField nameFields[] = {
            {"dateOrder", &updated.clock.dateOrder,
             [](const std::string& n) {
                 return std::string(apps::dateOrderName(apps::dateOrderFromName(n)));
             },
             "'clock.dateOrder' must be dayMonthYear, monthDayYear or yearMonthDay"},
            {"dateSeparator", &updated.clock.dateSeparator,
             [](const std::string& n) {
                 return std::string(apps::dateSeparatorName(apps::dateSeparatorFromName(n)));
             },
             "'clock.dateSeparator' must be dot, slash or dash"},
            {"dateYear", &updated.clock.dateYear,
             [](const std::string& n) {
                 return std::string(apps::dateYearName(apps::dateYearFromName(n)));
             },
             "'clock.dateYear' must be none, twoDigit or fourDigit"},
        };
        for (const NameField& field : nameFields) {
            const json::Value value = clock[field.key];
            if (!value.isString()) {
                continue;
            }
            const std::string name = value.toString();
            if (field.canonical(name) != name) {
                return unprocessable(field.complaint);
            }
            *field.target = name;
        }

        const struct {
            const char* key;
            std::uint32_t* target;
            const char* complaint;
        } colorFields[] = {
            {"color", &updated.clock.color, "'clock.color' must be #RRGGBB"},
            {"accentColor", &updated.clock.accentColor, "'clock.accentColor' must be #RRGGBB"},
            {"dateColor", &updated.clock.dateColor, "'clock.dateColor' must be #RRGGBB"},
        };
        for (const auto& field : colorFields) {
            const json::Value value = clock[field.key];
            if (!value.isString()) {
                continue;
            }
            Rgb parsedColor;
            if (!parseHexColor(value.raw(), parsedColor)) {
                return unprocessable(field.complaint);
            }
            *field.target = toPacked(parsedColor);
        }

        if (const json::Value blink = clock["blinkPeriodMillis"]; blink.isNumber()) {
            const std::int64_t value = blink.toInt(-1);
            // 0 is meaningful — it holds the colon lit rather than blinking.
            if (value != 0 && (value < 100 || value > 60000)) {
                return unprocessable(
                    "'clock.blinkPeriodMillis' must be 0 to hold the colon lit, or 100-60000");
            }
            updated.clock.blinkPeriodMillis = static_cast<std::uint32_t>(value);
        }
    }

    *context_.config = updated;

    if (context_.configStore != nullptr && !context_.configStore->save(updated)) {
        return serverError("settings applied but could not be saved");
    }
    if (context_.platform != nullptr) {
        context_.platform->display().setBrightness(updated.display.brightness);
        if (context_.platform->audio() != nullptr) {
            context_.platform->audio()->setVolume(
                config::volumeToByte(updated.audio.volumePercent));
        }
    }

    JsonWriter writer;
    writeSettings(writer, *context_.config);
    return ok(writer.take());
}

Response ApiServer::handleNetwork(const Request& request) {
    if (request.method != Method::Get) {
        return methodNotAllowed();
    }
    if (context_.platform == nullptr || context_.platform->network() == nullptr) {
        return error(501, "not_supported", "this platform has no network interface");
    }

    platform::INetworkManager& network = *context_.platform->network();
    const platform::NetworkStatus status = network.status();

    JsonWriter writer;
    writer.beginObject()
        .member("connected", status.connected)
        .member("ipv4", status.ipv4)
        .member("hostname", status.hostname);
    if (!status.ssid.empty()) {
        writer.member("ssid", status.ssid);
    }
    if (status.signalKnown) {
        writer.member("rssiDbm", status.rssiDbm);
    }

    // The lease, where something is managing one. Its absence is the
    // interesting case and is reported as absence: a device running on an
    // address nothing is renewing looks identical to a healthy one right
    // up until the address is taken back.
    writer.member("leaseManaged", status.leaseKnown);
    if (status.leaseKnown) {
        writer.member("leaseState", status.leaseState);
        if (status.leaseSeconds == 0xFFFFFFFFu) {
        writer.member("leaseSeconds", -1);  // granted forever
        } else {
        writer.member("leaseSeconds", static_cast<std::int64_t>(status.leaseSeconds));
        }
    }

    // Reported so a page can tell "this device cannot look" from "nothing is
    // in range" - which are different answers and look identical in an empty
    // list (ADR 0013).
    writer.member("canScan", network.canScan());
    writer.member("canJoin", network.canJoin());

    // False means the list below is remembered from before the radio became
    // an access point, not what is in range now. One radio cannot do both,
    // and the moment somebody needs to pick a network is exactly when the
    // device is hosting one.
    writer.member("networksAreLive", network.networksAreLive());

    const platform::INetworkManager::JoinProgress join = network.joinProgress();
    if (join.stage != platform::INetworkManager::JoinProgress::Stage::Idle) {
        const char* stage = "working";
        if (join.stage == platform::INetworkManager::JoinProgress::Stage::Succeeded) {
            stage = "succeeded";
        } else if (join.stage == platform::INetworkManager::JoinProgress::Stage::Failed) {
            stage = "failed";
        }
        writer.key("join").beginObject()
            .member("stage", stage)
            .member("ssid", join.ssid)
            .member("detail", join.detail)
            .endObject();
    }

    // The networks it will join on its own, in use or as a fallback. Names
    // only: what is remembered is never read back with its password.
    writer.member("canRemember", network.canRemember());
    writer.key("remembered").beginArray();
    for (const platform::INetworkManager::RememberedNetwork& held : network.rememberedNetworks()) {
        writer.beginObject()
            .member("ssid", held.ssid)
            .member("current", held.current)
            .endObject();
    }
    writer.endArray();

    writer.key("networks").beginArray();
    for (const platform::WirelessNetwork& found : network.networks()) {
        writer.beginObject()
            .member("ssid", found.ssid)
            .member("signalDbm", found.signalDbm)
            .member("secured", found.secured)
            .member("current", found.current)
            .endObject();
    }
    writer.endArray();

    writer.endObject();
    return ok(writer.take());
}

Response ApiServer::handleNetworkScan(const Request& request) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.platform == nullptr || context_.platform->network() == nullptr) {
        return error(501, "not_supported", "this platform has no network interface");
    }

    platform::INetworkManager& network = *context_.platform->network();
    if (!network.canScan()) {
        return error(501, "not_supported", "this platform cannot scan");
    }
    if (!network.beginScan()) {
        return error(503, "unavailable", "the radio would not start a scan");
    }

    // 202: a scan takes seconds, and blueprint §16 does not allow waiting for
    // it here. The caller asks again for the results.
    JsonWriter writer;
    writer.beginObject().member("status", "scanning").endObject();
    Response response = ok(writer.take());
    response.status = 202;
    return response;
}

Response ApiServer::handleNetworkRemember(const Request& request, bool remember) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.platform == nullptr || context_.platform->network() == nullptr) {
        return error(501, "not_supported", "this platform has no network interface");
    }
    platform::INetworkManager& network = *context_.platform->network();
    if (!network.canRemember()) {
        return error(409, "unavailable",
                     "networks can be remembered only while connected to one");
    }
    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }
    const json::Value root = body.root();
    if (!root.isObject() || !root["ssid"].isString()) {
        return badRequest("'ssid' is required");
    }
    const std::string ssid = root["ssid"].toString();
    std::string why;
    bool done = false;
    if (remember) {
        const json::Value passwordValue = root["password"];
        if (passwordValue.valid() && !passwordValue.isNull() && !passwordValue.isString()) {
            return badRequest("'password' must be a string");
        }
        done = network.rememberNetwork(
            ssid, passwordValue.isString() ? passwordValue.toString() : std::string(), why);
    } else {
        done = network.forgetNetwork(ssid, why);
    }
    if (!done) {
        return unprocessable(why);
    }
    // The password is not echoed, for the reason handleNetworkJoin gives.
    JsonWriter writer;
    writer.beginObject()
        .member("status", remember ? "remembered" : "forgotten")
        .member("ssid", ssid)
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleNetworkJoin(const Request& request) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.platform == nullptr || context_.platform->network() == nullptr) {
        return error(501, "not_supported", "this platform has no network interface");
    }

    platform::INetworkManager& network = *context_.platform->network();
    if (!network.canJoin()) {
        return error(501, "not_supported", "this platform cannot join networks");
    }

    Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
    if (!body.valid()) {
        return badRequest(std::string("invalid JSON: ") + body.errorText());
    }

    const json::Value root = body.root();
    if (!root.isObject()) {
        return badRequest("body must be a JSON object");
    }

    const json::Value ssidValue = root["ssid"];
    if (!ssidValue.isString()) {
        return badRequest("'ssid' is required");
    }
    const std::string ssid = ssidValue.toString();

    // Absent means an open network, which is a real thing and not the same as
    // a forgotten field. The reply says which was assumed, so a mistyped key
    // does not look like a successful join to an open network.
    const json::Value passwordValue = root["password"];
    if (!passwordValue.isNull() && !passwordValue.isString()) {
        return badRequest("'password' must be a string");
    }
    const std::string password = passwordValue.isString() ? passwordValue.toString()
                                                          : std::string();

    if (!network.beginJoin(ssid, password)) {
        // The reason lives in the progress, because it is written for the
        // person who typed the password rather than for a log.
        return error(400, "invalid_request", network.joinProgress().detail);
    }

    // 202, and the password is not echoed back - not even redacted. A
    // settings page that repeats a Wi-Fi password is one screenshot away
    // from giving it away, and backups are taken from this API (§22).
    JsonWriter writer;
    writer.beginObject()
        .member("status", "joining")
        .member("ssid", ssid)
        .member("secured", !password.empty())
        .endObject();
    Response response = ok(writer.take());
    response.status = 202;
    return response;
}

Response ApiServer::handleFirmware(const Request& request) {
    if (context_.platform == nullptr) {
        return serverError("no platform");
    }
    platform::IUpgradeManager* upgrade = context_.platform->upgrade();
    if (upgrade == nullptr) {
        return error(501, "not_supported", "this platform cannot install firmware");
    }

    if (request.method == Method::Get) {
        JsonWriter writer;
        writer.beginObject()
            .member("path", upgrade->applicationPath())
            .member("installedBytes", static_cast<std::int64_t>(upgrade->installedBytes()))
            .member("canRollBack", upgrade->hasPrevious())
            .member("maxBytes", static_cast<std::int64_t>(options_.maxImageBytes))
            // The version of the process answering, which is not necessarily
            // the version of the file named by `path` - see `restartPending`.
            .member("version", std::string(kVersion))
            .member("restartPending", upgrade->restartPending())
            .endObject();
        return ok(writer.take());
    }

    if (request.method == Method::Delete) {
        std::string problem;
        if (!upgrade->rollback(problem)) {
            return unprocessable(problem);
        }
        JsonWriter writer;
        writer.beginObject()
            .member("status", "rolled-back")
            .member("rebootRequired", true)
            .member("restartPending", upgrade->restartPending())
            .member("note", "The previous version is back. Reboot to run it.")
            .endObject();
        return ok(writer.take());
    }

    if (request.method != Method::Post) {
        return methodNotAllowed();
    }

    // Checked before a byte is written, because the asymmetry is brutal: the
    // cost of rejecting a good file is somebody uploading it again, and the
    // cost of accepting a bad one is a device that stops being able to tell
    // you about it. A build for the wrong architecture is the easy mistake -
    // it happened during bring-up, verified perfectly, and failed at dlopen
    // where only ADB could see it.
    const update::elf::ElfVerdict verdict = update::elf::inspect(request.body);
    if (verdict != update::elf::ElfVerdict::Ok) {
        return unprocessable(std::string("that file is ") +
                             update::elf::describe(verdict));
    }

    std::string problem;
    if (!upgrade->install(request.body, problem)) {
        return error(503, "unavailable",
                     problem.empty() ? "could not install the firmware" : problem);
    }

    JsonWriter writer;
    writer.beginObject()
        .member("status", "installed")
        .member("path", upgrade->applicationPath())
        .member("bytes", static_cast<std::int64_t>(request.body.size()))
        .member("canRollBack", upgrade->hasPrevious())
        .member("rebootRequired", true)
        .member("restartPending", upgrade->restartPending())
        // The version still running, so a page can say which one it is
        // replacing rather than leaving somebody to guess.
        .member("runningVersion", std::string(kVersion))
        // Said plainly, because "installed" could otherwise be read as
        // "running", and the difference is a reboot.
        .member("note",
                "Written, not yet running. Reboot to start it. If it will not "
                "load, the device falls back to the version flashed with it "
                "rather than to nothing.")
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleReset(const Request& request, std::uint64_t nowMillis) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.config == nullptr || context_.configStore == nullptr) {
        return serverError("configuration unavailable");
    }

    // "apps": true also clears installed apps and stored icons. Off by default,
    // and deliberately a separate flag rather than a second endpoint: somebody
    // resetting settings to sort out a display problem should not silently lose
    // the apps an integration spent a week pushing.
    bool includeApps = false;
    if (!request.body.empty()) {
        Body body(request.body, options_.maxJsonTokens, options_.maxBodyBytes);
        if (!body.valid()) {
            return badRequest(std::string("invalid JSON: ") + body.errorText());
        }
        const json::Value fields = body.root();
        if (!fields.isObject()) {
            return badRequest("body must be a JSON object");
        }
        includeApps = fields["apps"].toBool(false);
    }

    // The device name survives. It is how somebody tells one of these from
    // another on the network, it is not a setting that can be "wrong", and
    // losing it means finding the device again before you can fix whatever you
    // were resetting.
    const std::string name = context_.config->deviceName;

    // So does the app arrangement, unless the apps go too. It belongs with the
    // apps rather than with the settings - and clearing it here cleared only
    // the stored copy, leaving the running device in the user's order until it
    // next rebooted and silently reverted. A reset that takes effect at an
    // unpredictable point in the future is worse than one that does nothing.
    std::vector<config::AppPreference> order;
    if (!includeApps) {
        order = context_.config->apps.order;
    }

    *context_.config = config::Config{};
    context_.config->deviceName = name;
    context_.config->apps.order = std::move(order);

    if (includeApps && context_.apps != nullptr) {
        // System apps survive clear(), which is what guarantees the panel still
        // shows something afterwards.
        context_.apps->clear();
        if (context_.icons != nullptr) {
            context_.icons->clear();
        }
        if (context_.carousel != nullptr) {
            context_.carousel->tick(nowMillis);
        }
    }

    if (!context_.configStore->save(*context_.config)) {
        // Reported rather than swallowed: the running device is now on
        // defaults either way, and a caller that believes the reset persisted
        // when it did not will be surprised by the next boot.
        return serverError("settings reset but could not be saved");
    }

    JsonWriter writer;
    writer.beginObject()
        .member("status", "reset")
        .member("apps", includeApps)
        .endObject();
    return ok(writer.take());
}

Response ApiServer::handleReboot(const Request& request) {
    if (request.method != Method::Post) {
        return methodNotAllowed();
    }
    if (context_.platform == nullptr || context_.platform->rebooter() == nullptr) {
        // 501, not 500: the request was fine, this build simply cannot do it.
        return error(501, "not_supported", "this platform cannot reboot itself");
    }

    context_.platform->rebooter()->reboot();

    JsonWriter writer;
    writer.beginObject().member("status", "rebooting").endObject();

    Response response = ok(writer.take());
    response.status = 202;  // accepted: the reboot happens after we reply
    return response;
}

}  // namespace api
}  // namespace stipple
