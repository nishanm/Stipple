// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/api/ApiServer.h"

#include <string>
#include <vector>

#include "stipple/api/JsonWriter.h"
#include "stipple/core/Base64.h"
#include "stipple/app/Carousel.h"
#include "stipple/config/Config.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/json/Json.h"
#include "stipple/notify/Notifications.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "stipple/asset/IconStore.h"
#include "support/TestFramework.h"

using stipple::api::ApiContext;
using stipple::api::ApiOptions;
using stipple::api::ApiServer;
using stipple::api::JsonWriter;
using stipple::api::matchRoute;
using stipple::api::Method;
using stipple::api::methodFromName;
using stipple::api::Request;
using stipple::api::Resource;
using stipple::api::Response;
using stipple::app::AppRegistry;
using stipple::app::Carousel;
using stipple::config::Config;
using stipple::config::ConfigStore;
using stipple::notify::NotificationQueue;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

/// Owns a whole device's worth of state plus the server in front of it.
/// Records what the input endpoint injected, so a test can assert the events
/// that would have reached the mapper rather than their side effects.
struct RecordingInput : stipple::platform::IInputSink {
    std::vector<stipple::platform::InputEvent> events;
    void inject(const stipple::platform::InputEvent& event) override {
        events.push_back(event);
    }
};

struct Fixture {
    /// Optional capabilities, so a test can ask for a device that genuinely
    /// lacks one. Defaulted, so every existing call site is unchanged.
    SimulatorPlatform platform;
    AppRegistry apps;
    Carousel carousel{apps};
    NotificationQueue notifications;
    Config config;
    ConfigStore configStore{platform.storage()};
    stipple::Framebuffer framebuffer;
    stipple::asset::IconStore icons;
    RecordingInput input;
    ApiServer server;

    explicit Fixture(ApiOptions options = ApiOptions{})
        : server(makeContext(), std::move(options)) {}

    Fixture(stipple::platform::simulator::SimulatorCapabilities capabilities,
            ApiOptions options)
        : platform(capabilities), server(makeContext(), std::move(options)) {}

    ApiContext makeContext() {
        ApiContext context;
        context.apps = &apps;
        context.carousel = &carousel;
        context.notifications = &notifications;
        context.config = &config;
        context.configStore = &configStore;
        context.platform = &platform;
        context.frame = &framebuffer;
        context.input = &input;
        context.icons = &icons;
        return context;
    }

    Response call(const char* method,
                  const char* path,
                  const std::string& body = {},
                  const std::string& token = {},
                  std::uint64_t nowMillis = 0) {
        Request request;
        request.method = methodFromName(method);
        request.path = path;
        request.body = body;
        request.authToken = token;
        return server.handle(request, nowMillis);
    }

    void addApp(const std::string& id, const std::string& scene = R"({"elements":[]})") {
        stipple::app::App entry;
        entry.id = id;
        entry.name = id;
        entry.sceneJson = scene;
        apps.put(std::move(entry));
    }
};

/// Parse a response body so assertions read as field checks rather than string
/// matching, which would break on harmless formatting changes.
struct Parsed {
    stipple::json::Token tokens[512];
    std::string text;
    stipple::json::Document document{tokens, 512};
    bool ok = false;

    explicit Parsed(std::string body) : text(std::move(body)) {
        ok = document.parse(text) == stipple::json::Error::None;
    }
    stipple::json::Value root() const { return document.root(); }
};

}  // namespace

// --- routing -----------------------------------------------------------------

STIPPLE_TEST(Api, RoutesKnownPaths) {
    STIPPLE_CHECK(matchRoute("/api/v1/device").resource == Resource::Device);
    STIPPLE_CHECK(matchRoute("/api/v1/health").resource == Resource::Health);
    STIPPLE_CHECK(matchRoute("/api/v1/apps").resource == Resource::AppCollection);
    STIPPLE_CHECK(matchRoute("/api/v1/apps/clock").resource == Resource::AppItem);
    STIPPLE_CHECK(matchRoute("/api/v1/apps/clock/activate").resource == Resource::AppActivate);
    STIPPLE_CHECK(matchRoute("/api/v1/notifications").resource == Resource::NotificationCollection);
    STIPPLE_CHECK(matchRoute("/api/v1/notifications/x").resource == Resource::NotificationItem);
    STIPPLE_CHECK(matchRoute("/api/v1/settings").resource == Resource::Settings);
    STIPPLE_CHECK(matchRoute("/api/v1/display/frame").resource == Resource::DisplayFrame);
    STIPPLE_CHECK(matchRoute("/api/v1/input").resource == Resource::Input);
    STIPPLE_CHECK(matchRoute("/api/v1/system/reboot").resource == Resource::SystemReboot);
}

STIPPLE_TEST(Api, ExtractsPathIds) {
    STIPPLE_CHECK_EQ(matchRoute("/api/v1/apps/living-room").id, std::string("living-room"));
    STIPPLE_CHECK_EQ(matchRoute("/api/v1/apps/a/activate").id, std::string("a"));
}

STIPPLE_TEST(Api, TrailingSlashIsNotADifferentResource) {
    STIPPLE_CHECK(matchRoute("/api/v1/apps/").resource == Resource::AppCollection);
    STIPPLE_CHECK(matchRoute("//api//v1//apps//").resource == Resource::AppCollection);
}

STIPPLE_TEST(Api, RejectsUnknownPaths) {
    STIPPLE_CHECK(matchRoute("/").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/api/v2/device").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/api/v1").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/api/v1/nope").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/api/v1/apps/a/b/c").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/../../etc/passwd").resource == Resource::Unknown);
}

STIPPLE_TEST(Api, UnknownEndpointReturns404) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/nope").status, 404);
}

STIPPLE_TEST(Api, UnservedApiVersionPointsAtTheOneWeServe) {
    // A caller asking for an API version this device does not serve should be
    // told which one it does, rather than getting a dead end.
    Fixture fixture;

    const Response response = fixture.call("GET", "/api/v2/device");
    STIPPLE_CHECK_EQ(response.status, 404);
    STIPPLE_CHECK(response.body.find("/api/v1") != std::string::npos);

    STIPPLE_CHECK(fixture.call("GET", "/api/device").body.find("/api/v1") != std::string::npos);

    // A genuinely unknown path inside the served version keeps the plain
    // message: the version is fine, the endpoint simply does not exist.
    STIPPLE_CHECK(fixture.call("GET", "/api/v1/nope").body.find("no such endpoint") !=
                 std::string::npos);
    STIPPLE_CHECK(fixture.call("GET", "/api/v1/nope").body.find("version") == std::string::npos);
}

STIPPLE_TEST(Api, WrongMethodReturns405) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/device").status, 405);
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/health").status, 405);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/system/reboot").status, 405);
}

STIPPLE_TEST(Api, UnsupportedMethodReturns400) {
    Fixture fixture;
    Request request;
    request.method = Method::Unknown;
    request.path = "/api/v1/device";
    STIPPLE_CHECK_EQ(fixture.server.handle(request, 0).status, 400);
}

// --- authentication ----------------------------------------------------------

STIPPLE_TEST(Api, NoTokenConfiguredMeansOpenAccess) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/device").status, 200);
}

STIPPLE_TEST(Api, ConfiguredTokenIsRequired) {
    ApiOptions options;
    options.authToken = "s3cret";
    Fixture fixture(options);

    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/device").status, 401);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/device", "", "wrong").status, 401);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/device", "", "s3cret").status, 200);
}

STIPPLE_TEST(Api, HealthIsAlsoProtected) {
    // An unauthenticated liveness probe would leak uptime, version and app
    // names to anything on the LAN.
    ApiOptions options;
    options.authToken = "s3cret";
    Fixture fixture(options);

    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/health").status, 401);
}

STIPPLE_TEST(Api, UnauthenticatedCallersCannotEnumerateEndpoints) {
    // Both a real and a bogus path must answer 401, or the status difference
    // becomes a map of the API.
    ApiOptions options;
    options.authToken = "s3cret";
    Fixture fixture(options);

    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/settings").status, 401);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/nope").status, 404);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/apps/does-not-exist").status, 401);
}

// --- limits ------------------------------------------------------------------

STIPPLE_TEST(Api, OversizedBodyIsRejectedBeforeParsing) {
    ApiOptions options;
    options.maxBodyBytes = 64;
    Fixture fixture(options);

    const std::string huge(200, 'x');
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", huge).status, 413);
}

STIPPLE_TEST(Api, MalformedJsonIsRejected) {
    Fixture fixture;
    const Response response = fixture.call("POST", "/api/v1/apps", "{not json");
    STIPPLE_CHECK_EQ(response.status, 400);

    Parsed parsed(response.body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["error"]["code"].stringEquals("bad_request"));
}

STIPPLE_TEST(Api, NonObjectBodyIsRejected) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", "[1,2,3]").status, 400);
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", "\"text\"").status, 400);
}

STIPPLE_TEST(Api, ErrorsShareOneShape) {
    Fixture fixture;
    Parsed parsed(fixture.call("GET", "/api/v1/nope").body);

    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["error"].isObject());
    STIPPLE_CHECK(parsed.root()["error"]["code"].isString());
    STIPPLE_CHECK(parsed.root()["error"]["message"].isString());
}

// --- device information ------------------------------------------------------

STIPPLE_TEST(Api, DeviceReportsIdentityAndDisplay) {
    Fixture fixture;
    Parsed parsed(fixture.call("GET", "/api/v1/device").body);

    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["platform"].stringEquals("simulator"));
    STIPPLE_CHECK_EQ(parsed.root()["display"]["width"].toInt(), std::int64_t(52));
    STIPPLE_CHECK_EQ(parsed.root()["display"]["height"].toInt(), std::int64_t(16));
    STIPPLE_CHECK_EQ(parsed.root()["apiVersion"].toInt(), std::int64_t(1));
}

STIPPLE_TEST(Api, AbsentCapabilityIsNullNotFabricated) {
    // A platform with no network is different from one whose network is down.
    stipple::platform::simulator::SimulatorCapabilities none;
    none.network = false;

    SimulatorPlatform platform(none);
    ApiContext context;
    context.platform = &platform;
    ApiServer server(context);

    Request request;
    request.method = Method::Get;
    request.path = "/api/v1/device";

    Parsed parsed(server.handle(request, 0).body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["network"].isNull());
}

STIPPLE_TEST(Api, HealthReportsCounts) {
    Fixture fixture;
    fixture.addApp("a");
    fixture.addApp("b");

    Parsed parsed(fixture.call("GET", "/api/v1/health").body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["status"].stringEquals("ok"));
    STIPPLE_CHECK_EQ(parsed.root()["apps"].toInt(), std::int64_t(2));
}

STIPPLE_TEST(Api, DiagnosticsLeaksNoSecrets) {
    ApiOptions options;
    options.authToken = "super-secret-token";
    Fixture fixture(options);

    const Response response = fixture.call("GET", "/api/v1/diagnostics", "", "super-secret-token");
    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(response.body.find("super-secret-token") == std::string::npos);
    STIPPLE_CHECK(response.body.find("token") == std::string::npos);
}

// --- apps --------------------------------------------------------------------

STIPPLE_TEST(Api, ListsAppsInDisplayOrder) {
    Fixture fixture;
    fixture.addApp("zulu");
    fixture.addApp("alpha");

    Parsed parsed(fixture.call("GET", "/api/v1/apps").body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK_EQ(parsed.root()["count"].toInt(), std::int64_t(2));
    STIPPLE_CHECK(parsed.root()["apps"][0]["id"].stringEquals("zulu"));
    STIPPLE_CHECK(parsed.root()["apps"][1]["id"].stringEquals("alpha"));
    STIPPLE_CHECK_EQ(parsed.root()["apps"][1]["position"].toInt(), std::int64_t(1));
}

STIPPLE_TEST(Api, CreatesAnApp) {
    Fixture fixture;
    const Response response = fixture.call("POST", "/api/v1/apps",
        R"({"id":"weather","name":"Weather","durationSeconds":9,
            "scene":{"elements":[{"type":"pixel","x":0,"y":0}]}})");

    STIPPLE_CHECK_EQ(response.status, 201);
    STIPPLE_CHECK(fixture.apps.find("weather") != nullptr);
    STIPPLE_CHECK_EQ(fixture.apps.find("weather")->durationSeconds, 9);

    // The stored scene round-trips as JSON rather than as an escaped string.
    Parsed parsed(response.body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["scene"]["elements"].isArray());
}

STIPPLE_TEST(Api, CreateRequiresAnId) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", R"({"name":"nameless"})").status, 422);
}

STIPPLE_TEST(Api, CreateRejectsADuplicate) {
    Fixture fixture;
    fixture.addApp("clock");
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", R"({"id":"clock"})").status, 409);
}

STIPPLE_TEST(Api, SceneMustBeAnObject) {
    // Rejecting rather than guessing: a stringified scene is a client bug and
    // silently accepting it would make two clients mean different things.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/apps", R"({"id":"a","scene":"{\"elements\":[]}"})").status,
        422);
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps", R"({"id":"b","scene":[1,2]})").status,
                    422);
}

STIPPLE_TEST(Api, GetsASingleApp) {
    Fixture fixture;
    fixture.addApp("clock");

    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/apps/clock").status, 200);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/apps/missing").status, 404);
}

STIPPLE_TEST(Api, PutReplacesAndKeepsPosition) {
    Fixture fixture;
    fixture.addApp("a");
    fixture.addApp("b");
    fixture.addApp("c");

    const Response response =
        fixture.call("PUT", "/api/v1/apps/b", R"({"name":"renamed","durationSeconds":3})");

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK_EQ(fixture.apps.indexOf("b"), 1);
    STIPPLE_CHECK_EQ(fixture.apps.find("b")->name, std::string("renamed"));
}

STIPPLE_TEST(Api, PutIgnoresAnIdInTheBody) {
    // The URL is authoritative; otherwise PUT /apps/a could rename itself to b
    // and quietly clobber a different app.
    Fixture fixture;
    fixture.addApp("a");

    fixture.call("PUT", "/api/v1/apps/a", R"({"id":"hijacked","name":"x"})");
    STIPPLE_CHECK(fixture.apps.find("a") != nullptr);
    STIPPLE_CHECK(fixture.apps.find("hijacked") == nullptr);
}

STIPPLE_TEST(Api, PutCreatesWhenAbsent) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("PUT", "/api/v1/apps/new", R"({"name":"New"})").status, 201);
    STIPPLE_CHECK(fixture.apps.find("new") != nullptr);
}

STIPPLE_TEST(Api, PutWithoutSceneKeepsTheExistingOne) {
    Fixture fixture;
    fixture.addApp("a", R"({"elements":[{"type":"pixel","x":1,"y":1}]})");

    fixture.call("PUT", "/api/v1/apps/a", R"({"name":"renamed"})");
    STIPPLE_CHECK(fixture.apps.find("a")->sceneJson.find("pixel") != std::string::npos);
}

STIPPLE_TEST(Api, DeletesAnApp) {
    Fixture fixture;
    fixture.addApp("a");

    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/apps/a").status, 204);
    STIPPLE_CHECK(fixture.apps.find("a") == nullptr);
    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/apps/a").status, 404);
}

STIPPLE_TEST(Api, SystemAppsCannotBeDeleted) {
    // Something must still be on screen after a bad API call.
    Fixture fixture;
    stipple::app::App builtin;
    builtin.id = "clock";
    builtin.source = stipple::app::AppSource::System;
    fixture.apps.put(std::move(builtin));

    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/apps/clock").status, 409);
    STIPPLE_CHECK(fixture.apps.find("clock") != nullptr);
}

STIPPLE_TEST(Api, ActivatesAnApp) {
    Fixture fixture;
    fixture.addApp("a");
    fixture.addApp("b");
    fixture.carousel.tick(0);

    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps/b/activate", "", "", 500).status, 200);
    STIPPLE_CHECK_EQ(std::string(fixture.carousel.activeId()), std::string("b"));
}

STIPPLE_TEST(Api, ActivateRejectsUnknownAndDisabled) {
    Fixture fixture;
    fixture.addApp("a");
    fixture.apps.setEnabled("a", false);

    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps/missing/activate").status, 404);
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/apps/a/activate").status, 409);
}

// --- notifications -----------------------------------------------------------

STIPPLE_TEST(Api, PostsANotification) {
    Fixture fixture;
    const Response response = fixture.call("POST", "/api/v1/notifications",
        R"({"id":"bell","text":"Doorbell","priority":2,"durationSeconds":7})");

    STIPPLE_CHECK_EQ(response.status, 201);
    STIPPLE_CHECK(fixture.notifications.active() != nullptr);
    STIPPLE_CHECK_EQ(fixture.notifications.active()->text, std::string("Doorbell"));
}

STIPPLE_TEST(Api, NotificationRequiresText) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/notifications", R"({"id":"x"})").status, 422);
}

STIPPLE_TEST(Api, ListsNotificationState) {
    Fixture fixture;
    fixture.call("POST", "/api/v1/notifications", R"({"text":"one"})");

    Parsed parsed(fixture.call("GET", "/api/v1/notifications").body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["active"].isObject());
    STIPPLE_CHECK_EQ(parsed.root()["pending"].toInt(), std::int64_t(0));
}

STIPPLE_TEST(Api, EmptyQueueReportsNullActive) {
    Fixture fixture;
    Parsed parsed(fixture.call("GET", "/api/v1/notifications").body);
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK(parsed.root()["active"].isNull());
}

STIPPLE_TEST(Api, DismissesOneAndAll) {
    Fixture fixture;
    fixture.call("POST", "/api/v1/notifications", R"({"id":"a","text":"one"})");
    fixture.call("POST", "/api/v1/notifications", R"({"id":"b","text":"two"})");

    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/notifications/a").status, 204);
    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/notifications/nope").status, 404);
    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/notifications").status, 200);
    STIPPLE_CHECK_EQ(fixture.notifications.size(), 0);
}

STIPPLE_TEST(Api, SaturatedQueueAnswers429) {
    // Well-formed request, device simply full. Saying so beats pretending.
    Fixture fixture;
    fixture.call("POST", "/api/v1/notifications",
                 R"({"id":"hold","text":"held","priority":3,"durationSeconds":600})");
    for (int i = 0; i < NotificationQueue::kMaxQueued; ++i) {
        fixture.call("POST", "/api/v1/notifications",
                     R"({"text":"filler","priority":1})");
    }

    const Response response =
        fixture.call("POST", "/api/v1/notifications", R"({"text":"late","priority":0})");
    STIPPLE_CHECK_EQ(response.status, 429);
}

// --- settings ----------------------------------------------------------------

STIPPLE_TEST(Api, ReadsSettings) {
    Fixture fixture;
    Parsed parsed(fixture.call("GET", "/api/v1/settings").body);

    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK_EQ(parsed.root()["display"]["brightness"].toInt(), std::int64_t(128));
    STIPPLE_CHECK(parsed.root()["deviceName"].stringEquals("stipple"));
}

STIPPLE_TEST(Api, PatchUpdatesOnlyWhatIsSupplied) {
    Fixture fixture;
    fixture.config.deviceName = "kitchen";

    const Response response =
        fixture.call("PATCH", "/api/v1/settings", R"({"display":{"brightness":200}})");

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.display.brightness), 200);
    STIPPLE_CHECK_EQ(fixture.config.deviceName, std::string("kitchen"));  // untouched
}

STIPPLE_TEST(Api, PatchPersistsAndAppliesBrightness) {
    Fixture fixture;
    fixture.call("PATCH", "/api/v1/settings", R"({"display":{"brightness":64}})");

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.platform.display().brightness()), 64);

    Config reloaded;
    fixture.configStore.load(reloaded);
    STIPPLE_CHECK_EQ(static_cast<int>(reloaded.display.brightness), 64);
}

STIPPLE_TEST(Api, PatchRejectsOutOfRangeValues) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"display":{"brightness":999}})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"apps":{"defaultDurationSeconds":0}})")
            .status,
        422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"utcOffsetSeconds":999999}})")
            .status,
        422);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"deviceName":""})").status, 422);
}

STIPPLE_TEST(Api, ReportsClockStyleAsReadableText) {
    // Colours go out as #RRGGBB rather than integers: a settings UI can bind a
    // colour input to it directly, and a human reading the response can tell
    // what it is.
    Fixture fixture;
    fixture.config.clock.accentColor = 0x00BEFFu;
    Parsed parsed(fixture.call("GET", "/api/v1/settings").body);

    STIPPLE_CHECK(parsed.ok);
    const auto clock = parsed.root()["clock"];
    STIPPLE_CHECK(clock["accentColor"].stringEquals("#00BEFF"));
    STIPPLE_CHECK(clock["dateOrder"].stringEquals("dayMonthYear"));
    STIPPLE_CHECK(clock["dateYear"].stringEquals("none"));
    STIPPLE_CHECK_EQ(clock["blinkPeriodMillis"].toInt(), std::int64_t(1000));
}

STIPPLE_TEST(Api, PatchAcceptsClockStyle) {
    Fixture fixture;
    const Response response = fixture.call("PATCH", "/api/v1/settings",
                                           R"({"clock":{"color":"#FF8800",
                                                        "dateOrder":"monthDayYear",
                                                        "dateSeparator":"slash",
                                                        "dateYear":"fourDigit",
                                                        "leadingZero":false,
                                                        "blinkPeriodMillis":0}})");

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.clock.color), 0xFF8800);
    STIPPLE_CHECK_EQ(fixture.config.clock.dateOrder, std::string("monthDayYear"));
    STIPPLE_CHECK_EQ(fixture.config.clock.dateSeparator, std::string("slash"));
    STIPPLE_CHECK_EQ(fixture.config.clock.dateYear, std::string("fourDigit"));
    STIPPLE_CHECK_FALSE(fixture.config.clock.leadingZero);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.clock.blinkPeriodMillis), 0);
}

STIPPLE_TEST(Api, PatchRejectsUnknownClockNames) {
    // Silently falling back would leave a client believing it had selected
    // something it had not.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"dateOrder":"stardate"}})").status,
        422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"dateSeparator":"comma"}})").status,
        422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"dateYear":"roman"}})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"theme":"holographic"}})").status,
        422);
}

STIPPLE_TEST(Api, PatchRejectsMalformedColours) {
    Fixture fixture;
    const char* bad[] = {R"({"clock":{"color":"blue"}})", R"({"clock":{"color":"#FFF"}})",
                         R"({"clock":{"color":"#GGGGGG"}})",
                         R"({"clock":{"accentColor":"#1234567"}})"};
    for (const char* body : bad) {
        STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", body).status, 422);
    }
    // ...and the default survived every rejection.
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.clock.color), 0xFFFFFF);
}

STIPPLE_TEST(Api, PatchAcceptsZeroBlinkButNotAStrobe) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"blinkPeriodMillis":0}})").status,
        200);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"blinkPeriodMillis":5}})").status,
        422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"clock":{"blinkPeriodMillis":99999}})")
            .status,
        422);
}

STIPPLE_TEST(Api, SettingsSurviveAGetPatchGetCycle) {
    // Whatever GET reports must be acceptable to PATCH. If the two ever disagree
    // a settings page will start refusing values it just displayed.
    Fixture fixture;
    const std::string first = fixture.call("GET", "/api/v1/settings").body;
    Parsed parsed(first);
    STIPPLE_CHECK(parsed.ok);

    JsonWriter writer;
    writer.beginObject().key("clock").beginObject();
    writer.member("theme", parsed.root()["clock"]["theme"].toString());
    writer.member("color", parsed.root()["clock"]["color"].toString());
    writer.member("accentColor", parsed.root()["clock"]["accentColor"].toString());
    writer.member("dateColor", parsed.root()["clock"]["dateColor"].toString());
    writer.member("dateOrder", parsed.root()["clock"]["dateOrder"].toString());
    writer.member("dateSeparator", parsed.root()["clock"]["dateSeparator"].toString());
    writer.member("dateYear", parsed.root()["clock"]["dateYear"].toString());
    writer.endObject().endObject();

    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", writer.str()).status, 200);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/settings").body, first);
}

STIPPLE_TEST(Api, TheMqttPasswordIsWriteOnly) {
    // §22. A settings page needs to know whether a password is set; it must
    // never be able to read one back, and must not be handed a masked
    // placeholder it might helpfully save again.
    Fixture fixture;
    fixture.config.mqtt.password = "hunter2-do-not-leak";

    const std::string body = fixture.call("GET", "/api/v1/settings").body;

    STIPPLE_CHECK(body.find("hunter2-do-not-leak") == std::string::npos);
    STIPPLE_CHECK(body.find("\"passwordSet\":true") != std::string::npos);
    STIPPLE_CHECK(body.find("\"password\"") == std::string::npos);
}

STIPPLE_TEST(Api, TheMqttPasswordCanBeSetAndCleared) {
    Fixture fixture;

    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"password":"secret"}})").status, 200);
    STIPPLE_CHECK_EQ(fixture.config.mqtt.password, std::string("secret"));

    // An empty string is the only way to remove a stored credential.
    fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"password":""}})");
    STIPPLE_CHECK(fixture.config.mqtt.password.empty());
}

STIPPLE_TEST(Api, TheAccessPasswordIsWriteOnly) {
    // The same rule as the MQTT password, and for a sharper reason: backups
    // are taken from this API, so anything returned here ends up in a file
    // in somebody's downloads folder.
    Fixture fixture;
    fixture.config.web.username = "mark";
    fixture.config.web.password = "hunter2-do-not-leak";

    const std::string body = fixture.call("GET", "/api/v1/settings").body;

    STIPPLE_CHECK(body.find("hunter2-do-not-leak") == std::string::npos);
    STIPPLE_CHECK(body.find("\"username\":\"mark\"") != std::string::npos);
    STIPPLE_CHECK(body.find("\"passwordSet\":true") != std::string::npos);
}

STIPPLE_TEST(Api, AccessCanBeTurnedOnAndOff) {
    Fixture fixture;

    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings",
                     R"({"web":{"username":"mark","password":"hunter22"}})").status, 200);
    STIPPLE_CHECK_EQ(fixture.config.web.username, std::string("mark"));
    STIPPLE_CHECK_EQ(fixture.config.web.password, std::string("hunter22"));

    // Both cleared together, which is the only way off.
    fixture.call("PATCH", "/api/v1/settings", R"({"web":{"username":"","password":""}})");
    STIPPLE_CHECK(fixture.config.web.username.empty());
    STIPPLE_CHECK(fixture.config.web.password.empty());
}

STIPPLE_TEST(Api, RefusesHalfConfiguredAccess) {
    // The failure mode of getting this wrong is a device nobody can log
    // into, and unlike most settings the page that would fix it is behind
    // the thing that broke. So it is refused here rather than half-applied.
    Fixture fixture;

    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"web":{"username":"mark"}})").status, 422);
    STIPPLE_CHECK(fixture.config.web.username.empty());

    // And clearing only the password, leaving a username behind, is the same
    // mistake from the other direction.
    fixture.config.web.username = "mark";
    fixture.config.web.password = "hunter22";
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"web":{"password":""}})").status, 422);
    STIPPLE_CHECK_EQ(fixture.config.web.password, std::string("hunter22"));
}

STIPPLE_TEST(Api, RefusesAUsernameThatCouldNeverBeSent) {
    // A Basic credential is "user:password" split on the first colon, so a
    // username containing one could never come back. Accepting it would
    // store a setting that locks the device permanently.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings",
                     R"({"web":{"username":"ma:rk","password":"hunter22"}})").status, 422);
    STIPPLE_CHECK(fixture.config.web.username.empty());
}

STIPPLE_TEST(Api, PatchAcceptsMqttSettings) {
    Fixture fixture;
    const Response response = fixture.call("PATCH", "/api/v1/settings",
                                           R"({"mqtt":{"enabled":true,"host":"broker.local",
                                                       "port":8883,"baseTopic":"home",
                                                       "tls":true,"keepAliveSeconds":45}})");

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(fixture.config.mqtt.enabled);
    STIPPLE_CHECK_EQ(fixture.config.mqtt.host, std::string("broker.local"));
    STIPPLE_CHECK_EQ(fixture.config.mqtt.port, 8883);
    STIPPLE_CHECK_EQ(fixture.config.mqtt.baseTopic, std::string("home"));
    STIPPLE_CHECK(fixture.config.mqtt.tls);
}

STIPPLE_TEST(Api, PatchRejectsUnusableMqttSettings) {
    Fixture fixture;

    // A wildcard base topic would make the device publish to a filter, which no
    // broker accepts and which is miserable to diagnose from the other end.
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"baseTopic":"home/#"}})").status,
        422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"baseTopic":"a/+/b"}})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"baseTopic":""}})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"port":0}})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/settings", R"({"mqtt":{"keepAliveSeconds":1}})").status,
        422);
}

STIPPLE_TEST(Api, RejectedPatchLeavesSettingsUntouched) {
    // Applied to a copy, so a bad field cannot half-update the device.
    Fixture fixture;
    fixture.config.deviceName = "before";

    fixture.call("PATCH", "/api/v1/settings",
                 R"({"deviceName":"after","display":{"brightness":999}})");

    STIPPLE_CHECK_EQ(fixture.config.deviceName, std::string("before"));
}

// --- system ------------------------------------------------------------------

STIPPLE_TEST(Api, RebootIsAccepted) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/system/reboot").status, 202);
    STIPPLE_CHECK_EQ(fixture.platform.simulatedRebooter().rebootCount(), 1u);
}

STIPPLE_TEST(Api, RebootReports501WhenUnsupported) {
    stipple::platform::simulator::SimulatorCapabilities none;
    none.rebooter = false;
    SimulatorPlatform platform(none);

    ApiContext context;
    context.platform = &platform;
    ApiServer server(context);

    Request request;
    request.method = Method::Post;
    request.path = "/api/v1/system/reboot";
    STIPPLE_CHECK_EQ(server.handle(request, 0).status, 501);
}

// --- JSON writer -------------------------------------------------------------

STIPPLE_TEST(Api, JsonWriterEscapesAndNests) {
    JsonWriter writer;
    writer.beginObject()
        .member("quote", "he said \"hi\"")
        .member("newline", "a\nb")
        .member("count", 3)
        .member("flag", true)
        .key("list")
        .beginArray()
        .value("x")
        .value(std::int64_t(7))
        .endArray()
        .endObject();

    Parsed parsed(writer.take());
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK_EQ(parsed.root()["quote"].toString(), std::string("he said \"hi\""));
    STIPPLE_CHECK_EQ(parsed.root()["newline"].toString(), std::string("a\nb"));
    STIPPLE_CHECK_EQ(parsed.root()["list"][1].toInt(), std::int64_t(7));
}

STIPPLE_TEST(Api, JsonWriterHandlesEmptyContainers) {
    JsonWriter writer;
    writer.beginObject().key("empty").beginArray().endArray().key("obj").beginObject().endObject().endObject();

    Parsed parsed(writer.take());
    STIPPLE_CHECK(parsed.ok);
    STIPPLE_CHECK_EQ(parsed.root()["empty"].size(), 0);
}

STIPPLE_TEST(Api, EveryResponseBodyIsValidJson) {
    // Any malformed body would break every client at once, so sweep the surface.
    Fixture fixture;
    fixture.addApp("a");
    fixture.call("POST", "/api/v1/notifications", R"({"id":"n","text":"hi"})");

    const char* paths[] = {
        "/api/v1/device", "/api/v1/health", "/api/v1/version", "/api/v1/diagnostics",
        "/api/v1/apps",   "/api/v1/apps/a", "/api/v1/settings", "/api/v1/notifications",
        "/api/v1/nope",   "/api/v1/apps/missing",
    };

    for (const char* path : paths) {
        const Response response = fixture.call("GET", path);
        if (response.body.empty()) {
            continue;  // 204 carries no body
        }
        Parsed parsed(response.body);
        STIPPLE_CHECK(parsed.ok);
    }
}

// --- live view and injected input --------------------------------------------

STIPPLE_TEST(Api, TheFrameEndpointReturnsTheWholePanel) {
    Fixture fixture;
    fixture.framebuffer.fill(stipple::colors::kBlack);
    fixture.framebuffer.set(0, 0, stipple::rgb(255, 0, 0));
    fixture.framebuffer.set(51, 15, stipple::rgb(0, 0, 255));

    const Response response = fixture.call("GET", "/api/v1/display/frame");
    STIPPLE_CHECK_EQ(response.status, 200);

    // Base64 of 52*16*3 bytes, which must be the whole panel and not a crop.
    const std::size_t expected =
        stipple::base64::encodedSize(stipple::Framebuffer::kByteSize);
    const std::size_t open = response.body.find("\"pixels\":\"");
    STIPPLE_CHECK(open != std::string::npos);
    const std::size_t start = open + 10;
    const std::size_t close = response.body.find('"', start);
    STIPPLE_CHECK_EQ(close - start, expected);

    STIPPLE_CHECK(response.body.find("\"width\":52") != std::string::npos);
    STIPPLE_CHECK(response.body.find("\"height\":16") != std::string::npos);
    STIPPLE_CHECK(response.body.find("rgb888") != std::string::npos);
}

STIPPLE_TEST(Api, TheFrameEndpointIsReadOnly) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/display/frame").status, 405);
}

STIPPLE_TEST(Api, AButtonPressArrivesAsDownAndUp) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/input", R"({"control":"middle"})", {}, 5000).status,
        204);

    STIPPLE_CHECK_EQ(fixture.input.events.size(), std::size_t{2});
    STIPPLE_CHECK(fixture.input.events[0].source == stipple::platform::RawInput::KeyMiddle);
    STIPPLE_CHECK(fixture.input.events[0].phase == stipple::platform::ButtonPhase::Down);
    STIPPLE_CHECK(fixture.input.events[1].phase == stipple::platform::ButtonPhase::Up);
}

STIPPLE_TEST(Api, HoldMillisReachesTheMapperAsARealLongPress) {
    // The whole point of the field: without it a browser could only ever tap,
    // and half the default bindings are long presses.
    Fixture fixture;
    fixture.call("POST", "/api/v1/input", R"({"control":"plus","holdMillis":900})", {}, 1000);

    STIPPLE_CHECK_EQ(fixture.input.events.size(), std::size_t{2});
    STIPPLE_CHECK_EQ(fixture.input.events[1].timestampMillis -
                        fixture.input.events[0].timestampMillis,
                    std::uint64_t{900});
}

STIPPLE_TEST(Api, ADetentIsOneTickNotAPress) {
    Fixture fixture;
    fixture.call("POST", "/api/v1/input", R"({"control":"right"})", {}, 0);

    STIPPLE_CHECK_EQ(fixture.input.events.size(), std::size_t{1});
    STIPPLE_CHECK(fixture.input.events[0].source == stipple::platform::RawInput::RotaryRight);
    STIPPLE_CHECK(fixture.input.events[0].phase == stipple::platform::ButtonPhase::Tick);
}

STIPPLE_TEST(Api, AnUnknownControlIsRefusedRatherThanGuessed) {
    // A web button that silently pressed the wrong control would be worse than
    // one that did nothing at all.
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/input", R"({"control":"wheel"})").status, 422);
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/input", R"({})").status, 400);
    STIPPLE_CHECK(fixture.input.events.empty());
}

STIPPLE_TEST(Api, AnAbsurdHoldIsRefused) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/input", R"({"control":"plus","holdMillis":99999})").status,
        422);
    STIPPLE_CHECK(fixture.input.events.empty());
}

// --- enabling and disabling apps ---------------------------------------------

STIPPLE_TEST(Api, DisablingAnAppIsAPatchNotAReplace) {
    // The config page has always sent PATCH here. The route only handled GET,
    // DELETE and PUT, so every toggle in the UI answered 405 and the app stayed
    // exactly as it was.
    Fixture fixture;
    fixture.addApp("weather");

    const Response off =
        fixture.call("PATCH", "/api/v1/apps/weather", R"({"enabled":false})");
    STIPPLE_CHECK_EQ(off.status, 200);
    STIPPLE_CHECK_FALSE(fixture.apps.find("weather")->enabled);

    const Response on =
        fixture.call("PATCH", "/api/v1/apps/weather", R"({"enabled":true})");
    STIPPLE_CHECK_EQ(on.status, 200);
    STIPPLE_CHECK(fixture.apps.find("weather")->enabled);
}

STIPPLE_TEST(Api, PatchingLeavesEverythingItDoesNotName) {
    Fixture fixture;
    fixture.addApp("weather", R"({"elements":[]})");
    fixture.apps.find("weather")->name = "Weather";
    fixture.apps.find("weather")->durationSeconds = 12;

    fixture.call("PATCH", "/api/v1/apps/weather", R"({"enabled":false})");

    const stipple::app::App* entry = fixture.apps.find("weather");
    STIPPLE_CHECK_EQ(entry->name, std::string("Weather"));
    STIPPLE_CHECK_EQ(entry->durationSeconds, 12);
    STIPPLE_CHECK_FALSE(entry->sceneJson.empty());
}

STIPPLE_TEST(Api, PatchingSomethingAbsentIsNotFound) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/apps/ghost", R"({"enabled":false})").status,
                    404);
}

STIPPLE_TEST(Api, ASceneCannotBePatchedIn) {
    // Changing what an app *is* is a replace. Allowing it here would make PATCH
    // a second, subtly different way to create apps.
    Fixture fixture;
    fixture.addApp("weather");
    STIPPLE_CHECK_EQ(
        fixture.call("PATCH", "/api/v1/apps/weather", R"({"scene":{"elements":[]}})").status,
        422);
}

STIPPLE_TEST(Api, ReplacingASystemAppKeepsItABuiltin) {
    // A PUT that dropped `builtin` left an entry that existed, was enabled, and
    // rendered nothing - the clock silently replaced by a blank card.
    Fixture fixture;
    stipple::app::App clock;
    clock.id = "clock";
    clock.name = "Clock";
    clock.source = stipple::app::AppSource::System;
    clock.builtin = stipple::app::Builtin::Clock;
    fixture.apps.put(std::move(clock));

    fixture.call("PUT", "/api/v1/apps/clock", R"({"enabled":false})");
    STIPPLE_CHECK(fixture.apps.find("clock")->builtin == stipple::app::Builtin::Clock);

    fixture.call("PATCH", "/api/v1/apps/clock", R"({"enabled":true})");
    STIPPLE_CHECK(fixture.apps.find("clock")->builtin == stipple::app::Builtin::Clock);
    STIPPLE_CHECK(fixture.apps.find("clock")->source == stipple::app::AppSource::System);
}

STIPPLE_TEST(Api, AnAppCanBeMovedByPatchingItsPosition) {
    // Position is a property of the app like any other, so it moves on the
    // same verb rather than needing an endpoint of its own.
    Fixture fixture;
    fixture.addApp("one");
    fixture.addApp("two");
    fixture.addApp("three");

    const Response moved = fixture.call("PATCH", "/api/v1/apps/three", R"({"position":0})");
    STIPPLE_CHECK_EQ(static_cast<int>(moved.status), 200);

    STIPPLE_CHECK_EQ(fixture.apps.at(0)->id, std::string("three"));
    STIPPLE_CHECK_EQ(fixture.apps.at(1)->id, std::string("one"));
    STIPPLE_CHECK_EQ(fixture.apps.at(2)->id, std::string("two"));
}

STIPPLE_TEST(Api, MovingAnAppReportsItsNewPosition) {
    // The reply used to say position 0 whatever happened, which is a lie a client
    // would happily rebuild its list from.
    Fixture fixture;
    fixture.addApp("one");
    fixture.addApp("two");
    fixture.addApp("three");

    const Response moved = fixture.call("PATCH", "/api/v1/apps/one", R"({"position":2})");
    STIPPLE_CHECK_EQ(static_cast<int>(moved.status), 200);
    STIPPLE_CHECK(moved.body.find("\"position\":2") != std::string::npos);
}

STIPPLE_TEST(Api, APositionOutsideTheInstalledAppsIsRefused) {
    Fixture fixture;
    fixture.addApp("one");
    fixture.addApp("two");

    STIPPLE_CHECK_EQ(
        static_cast<int>(fixture.call("PATCH", "/api/v1/apps/one", R"({"position":9})").status), 422);
    STIPPLE_CHECK_EQ(
        static_cast<int>(fixture.call("PATCH", "/api/v1/apps/one", R"({"position":-1})").status), 422);

    // And nothing moved on the way to being refused.
    STIPPLE_CHECK_EQ(fixture.apps.at(0)->id, std::string("one"));
}

// --- reset -------------------------------------------------------------------

STIPPLE_TEST(Api, ResetPutsSettingsBackToDefaults) {
    Fixture fixture;
    fixture.config.display.brightness = 17;
    fixture.config.clock.theme = "calendar";
    fixture.config.apps.defaultDurationSeconds = 44;

    const Response reset = fixture.call("POST", "/api/v1/system/reset");
    STIPPLE_CHECK_EQ(static_cast<int>(reset.status), 200);

    const stipple::config::Config defaults;
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.display.brightness),
                    static_cast<int>(defaults.display.brightness));
    STIPPLE_CHECK_EQ(fixture.config.clock.theme, defaults.clock.theme);
    STIPPLE_CHECK_EQ(fixture.config.apps.defaultDurationSeconds,
                    defaults.apps.defaultDurationSeconds);
}

STIPPLE_TEST(Api, ResetKeepsTheDeviceName) {
    // It is how somebody tells one of these from another on the network, it is
    // not a setting that can be "wrong", and losing it means finding the device
    // again before you can fix whatever you were resetting.
    Fixture fixture;
    fixture.config.deviceName = "kitchen";
    fixture.config.display.brightness = 17;

    fixture.call("POST", "/api/v1/system/reset");

    STIPPLE_CHECK_EQ(fixture.config.deviceName, std::string("kitchen"));
}

STIPPLE_TEST(Api, ResetKeepsTheArrangementWithTheApps) {
    // Clearing the stored order cleared only the stored copy: the running
    // device stayed in the user's order until it next rebooted and then
    // silently reverted. A reset that takes effect at an unpredictable point
    // in the future is worse than one that does nothing.
    Fixture fixture;
    stipple::config::AppPreference arranged;
    arranged.id = "battery";
    fixture.config.apps.order.push_back(arranged);
    fixture.config.display.brightness = 17;

    fixture.call("POST", "/api/v1/system/reset");
    STIPPLE_REQUIRE(fixture.config.apps.order.size() == 1);
    STIPPLE_CHECK_EQ(fixture.config.apps.order[0].id, std::string("battery"));

    // And it goes when the apps do, because then there is nothing to arrange.
    fixture.call("POST", "/api/v1/system/reset", R"({"apps":true})");
    STIPPLE_CHECK(fixture.config.apps.order.empty());
}

STIPPLE_TEST(Api, ResetKeepsAppsUnlessAskedOtherwise) {
    // Somebody resetting settings to sort out a display problem should not
    // silently lose the apps an integration spent a week pushing.
    Fixture fixture;
    fixture.addApp("pushed");

    fixture.call("POST", "/api/v1/system/reset");
    STIPPLE_CHECK(fixture.apps.find("pushed") != nullptr);

    fixture.call("POST", "/api/v1/system/reset", R"({"apps":true})");
    STIPPLE_CHECK(fixture.apps.find("pushed") == nullptr);
}

STIPPLE_TEST(Api, ResetIsPersisted) {
    // The running device is on defaults either way; a caller that believes the
    // reset survived a reboot when it did not will be surprised later.
    Fixture fixture;
    fixture.config.display.brightness = 17;
    fixture.call("POST", "/api/v1/system/reset");

    stipple::config::Config stored;
    fixture.configStore.load(stored);
    const stipple::config::Config defaults;
    STIPPLE_CHECK_EQ(static_cast<int>(stored.display.brightness),
                    static_cast<int>(defaults.display.brightness));
}

STIPPLE_TEST(Api, ResetRefusesTheWrongMethod) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.call("GET", "/api/v1/system/reset").status), 405);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.call("DELETE", "/api/v1/system/reset").status), 405);
}

STIPPLE_TEST(Api, SettingsRoundTripThroughABackup) {
    // What the web UI's backup and restore actually do: read the settings
    // document, send the whole thing back, and get the same device state. If
    // any field the API emits is one it will not accept, this fails - which is
    // the only way to notice a write-only or read-only field before a user
    // does.
    Fixture fixture;
    fixture.config.deviceName = "hallway";
    fixture.config.display.brightness = 42;
    fixture.config.display.overlay = "confetti";
    fixture.config.clock.theme = "calendar";
    fixture.config.clock.timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
    fixture.config.apps.defaultDurationSeconds = 17;
    fixture.config.apps.transition = "dissolve";
    fixture.config.visualizer.style = "meter";
    fixture.config.notifications.sound = "alert";
    fixture.config.clock.tick = true;

    stipple::config::AppPreference arranged;
    arranged.id = "battery";
    arranged.enabled = false;
    arranged.durationSeconds = 9;
    fixture.config.apps.order.push_back(arranged);

    const Response saved = fixture.call("GET", "/api/v1/settings");
    STIPPLE_REQUIRE(saved.status == 200);
    const std::string backup = saved.body;

    // Wipe, then restore from the backup exactly as the page does.
    fixture.call("POST", "/api/v1/system/reset");
    const Response restored = fixture.call("PATCH", "/api/v1/settings", backup);
    STIPPLE_CHECK_EQ(static_cast<int>(restored.status), 200);

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.display.brightness), 42);
    STIPPLE_CHECK_EQ(fixture.config.display.overlay, std::string("confetti"));
    STIPPLE_CHECK_EQ(fixture.config.clock.theme, std::string("calendar"));
    STIPPLE_CHECK_EQ(fixture.config.clock.timezone,
                    std::string("CET-1CEST,M3.5.0,M10.5.0/3"));
    STIPPLE_CHECK_EQ(fixture.config.apps.defaultDurationSeconds, 17);
    STIPPLE_CHECK_EQ(fixture.config.apps.transition, std::string("dissolve"));
    STIPPLE_CHECK_EQ(fixture.config.visualizer.style, std::string("meter"));
    STIPPLE_CHECK_EQ(fixture.config.notifications.sound, std::string("alert"));
    STIPPLE_CHECK(fixture.config.clock.tick);

    // The arrangement too. It was persisted to flash and left out of this
    // document, so a backup silently omitted it and a restore could not bring
    // it back - and this test passed anyway until it started looking.
    STIPPLE_REQUIRE(fixture.config.apps.order.size() == 1);
    STIPPLE_CHECK_EQ(fixture.config.apps.order[0].id, std::string("battery"));
    STIPPLE_CHECK_FALSE(fixture.config.apps.order[0].enabled);
    STIPPLE_CHECK_EQ(fixture.config.apps.order[0].durationSeconds, 9);
}

STIPPLE_TEST(Api, AMalformedOrderIsRefusedRatherThanPartlyApplied) {
    Fixture fixture;

    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("PATCH", "/api/v1/settings",
                     R"({"apps":{"order":[{"enabled":true}]}})").status), 422);
    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("PATCH", "/api/v1/settings",
                     R"({"apps":{"order":["clock"]}})").status), 422);
    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("PATCH", "/api/v1/settings",
                     R"({"apps":{"order":[{"id":"a","durationSeconds":99999}]}})").status), 422);

    // Nothing was written on the way to being refused.
    STIPPLE_CHECK(fixture.config.apps.order.empty());
}

STIPPLE_TEST(Api, AButtonCanBeHeldAcrossRequests) {
    // Without this the only thing reachable from outside is a complete press,
    // which makes any two-button gesture untestable except by standing in
    // front of the device - and the rescue gesture is exactly that, on the one
    // path that has to work when nothing else does.
    Fixture fixture;

    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("POST", "/api/v1/input", R"({"control":"minus","phase":"down"})").status),
        204);
    STIPPLE_REQUIRE(fixture.input.events.size() == 1);
    STIPPLE_CHECK(fixture.input.events[0].phase == stipple::platform::ButtonPhase::Down);

    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("POST", "/api/v1/input", R"({"control":"minus","phase":"up"})").status),
        204);
    STIPPLE_REQUIRE(fixture.input.events.size() == 2);
    STIPPLE_CHECK(fixture.input.events[1].phase == stipple::platform::ButtonPhase::Up);
}

STIPPLE_TEST(Api, AnUnknownPhaseIsRefused) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(static_cast<int>(
        fixture.call("POST", "/api/v1/input", R"({"control":"minus","phase":"sideways"})").status),
        422);
    STIPPLE_CHECK(fixture.input.events.empty());
}

STIPPLE_TEST(Api, WithoutAPhaseAPressIsStillCompleteBothWays) {
    // The common case stays one request, because a web UI pressing a button
    // should not have to remember to let go.
    Fixture fixture;
    fixture.call("POST", "/api/v1/input", R"({"control":"plus"})");
    STIPPLE_REQUIRE(fixture.input.events.size() == 2);
    STIPPLE_CHECK(fixture.input.events[0].phase == stipple::platform::ButtonPhase::Down);
    STIPPLE_CHECK(fixture.input.events[1].phase == stipple::platform::ButtonPhase::Up);
}

// --- network -----------------------------------------------------------------

STIPPLE_TEST(Api, NetworkReportsWhatTheDeviceIsOn) {
    Fixture fixture;
    stipple::platform::NetworkStatus status;
    status.connected = true;
    status.ipv4 = "192.168.1.50";
    status.hostname = "stipple";
    status.ssid = "Example Network";
    status.signalKnown = true;
    status.rssiDbm = -51;
    fixture.platform.simulatedNetwork().setStatus(status);

    const Response answer = fixture.call("GET", "/api/v1/network");
    STIPPLE_CHECK_EQ(static_cast<int>(answer.status), 200);
    STIPPLE_CHECK(answer.body.find("\"ssid\":\"Example Network\"") != std::string::npos);
    STIPPLE_CHECK(answer.body.find("\"rssiDbm\":-51") != std::string::npos);
}

STIPPLE_TEST(Api, APlatformThatCannotScanSaysSoRatherThanReturningNothing) {
    // An empty list and "this device cannot look" are different answers, and
    // they are indistinguishable without saying which one this is (ADR 0013).
    Fixture fixture;

    const Response answer = fixture.call("GET", "/api/v1/network");
    STIPPLE_CHECK(answer.body.find("\"canScan\":false") != std::string::npos);
    STIPPLE_CHECK(answer.body.find("\"networks\":[]") != std::string::npos);

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.call("POST", "/api/v1/network/scan").status), 501);
}

STIPPLE_TEST(Api, ScanningIsAcceptedRatherThanWaitedFor) {
    // A scan takes seconds and §16 does not allow waiting for one here, so the
    // reply says it started and the caller asks again.
    Fixture fixture;
    fixture.platform.simulatedNetwork().setScannable(true);

    const Response answer = fixture.call("POST", "/api/v1/network/scan");
    STIPPLE_CHECK_EQ(static_cast<int>(answer.status), 202);
    STIPPLE_CHECK(answer.body.find("scanning") != std::string::npos);
}

STIPPLE_TEST(Api, ScanResultsComeBackThroughTheNetworkResource) {
    Fixture fixture;
    fixture.platform.simulatedNetwork().setScannable(true);

    stipple::platform::WirelessNetwork found;
    found.ssid = "Example Network";
    found.signalDbm = -42;
    found.secured = true;
    found.current = true;
    fixture.platform.simulatedNetwork().setNetworks({found});

    const Response answer = fixture.call("GET", "/api/v1/network");
    STIPPLE_CHECK(answer.body.find("\"canScan\":true") != std::string::npos);
    STIPPLE_CHECK(answer.body.find("\"signalDbm\":-42") != std::string::npos);
    STIPPLE_CHECK(answer.body.find("\"secured\":true") != std::string::npos);
    STIPPLE_CHECK(answer.body.find("\"current\":true") != std::string::npos);
}

STIPPLE_TEST(Api, NetworkRefusesTheWrongMethods) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.call("POST", "/api/v1/network").status), 405);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.call("GET", "/api/v1/network/scan").status), 405);
}

STIPPLE_TEST(Routes, FirmwareIsRoutedAndNothingElseUnderSystemIs) {
    using stipple::api::matchRoute;
    using stipple::api::Resource;

    STIPPLE_CHECK(matchRoute("/api/v1/system/firmware").resource == Resource::SystemFirmware);
    STIPPLE_CHECK(matchRoute("/api/v1/system/reboot").resource == Resource::SystemReboot);
    STIPPLE_CHECK(matchRoute("/api/v1/system/reset").resource == Resource::SystemReset);

    // The staging route is gone on purpose, not renamed by accident. It wrote
    // update.img to the USB volume, and the vendor's recovery daemon installs
    // whatever sits there unattended - it reverted a working STIPPLE on real
    // hardware. A 404 is the correct answer forever.
    STIPPLE_CHECK(matchRoute("/api/v1/system/restore-image").resource == Resource::Unknown);

    STIPPLE_CHECK(matchRoute("/api/v1/system").resource == Resource::Unknown);
    STIPPLE_CHECK(matchRoute("/api/v1/system/firmware/extra").resource == Resource::Unknown);
}

STIPPLE_TEST(Assets, AnIconCanBeFetchedBackWithItsPixels) {
    Fixture fixture;

    // A 2x2 icon, four distinct colours, so a transposed or reversed frame
    // would not survive the comparison.
    const char* upload =
        R"({"id":"quad","width":2,"height":2,"frames":[[16711680,65280,255,16777215]]})";
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/assets", upload).status, 201);

    const Response one = fixture.call("GET", "/api/v1/assets/quad");
    STIPPLE_CHECK_EQ(one.status, 200);

    // The point of returning them: an icon fetched from one device can be
    // posted to another without translation.
    STIPPLE_CHECK(one.body.find("\"pixels\"") != std::string::npos);
    STIPPLE_CHECK(one.body.find("16711680") != std::string::npos);
    STIPPLE_CHECK(one.body.find("16777215") != std::string::npos);
}

STIPPLE_TEST(Assets, ASixteenBySixteenIconFits) {
    // The limit people actually hit, and it was invisible when they did.
    //
    // Pixels arrive as JSON integers, so every pixel is a token. A 16x16
    // frame is 256 of them and the general budget was 512, which meant a
    // single static icon squeaked through and a two-frame animation did not -
    // on a device whose panel is sixteen pixels tall, and reported as
    // "invalid JSON: too many tokens" on an icon well inside every size limit
    // the page shows you.
    Fixture fixture;

    std::string upload = R"({"id":"big","width":16,"height":16,"frameMillis":120,"frames":[)";
    for (int frame = 0; frame < 4; ++frame) {
        upload += frame == 0 ? "[" : ",[";
        for (int i = 0; i < 256; ++i) {
            if (i > 0) { upload += ','; }
            upload += std::to_string((frame * 256 + i) & 0xFFFFFF);
        }
        upload += ']';
    }
    upload += "]}";

    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/assets", upload).status, 201);

    const Response one = fixture.call("GET", "/api/v1/assets/big");
    STIPPLE_CHECK_EQ(one.status, 200);
    STIPPLE_CHECK(one.body.find("\"frames\":4") != std::string::npos);
}

STIPPLE_TEST(Assets, AFullSizeAnimationStillFits) {
    // The largest geometry the store accepts: 32x32, sixteen frames, 16,384
    // pixels. If the budgets do not cover this, then the store's own limits
    // are advertising a size the API cannot carry.
    Fixture fixture;

    std::string upload = R"({"id":"max","width":32,"height":32,"frames":[)";
    for (int frame = 0; frame < 16; ++frame) {
        upload += frame == 0 ? "[" : ",[";
        for (int i = 0; i < 1024; ++i) {
            if (i > 0) { upload += ','; }
            upload += std::to_string(i & 0xFF);
        }
        upload += ']';
    }
    upload += "]}";

    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/assets", upload).status, 201);
}

STIPPLE_TEST(Assets, TheBudgetIsStillABudget) {
    // Raised, not removed. Sixteen 32x32 animations is 786 KB of pixels and
    // the store holds 256 KB, so somewhere in there it has to say no - and it
    // has to say which no, because "too many icons" and "out of room" are
    // different problems with different fixes.
    Fixture fixture;

    std::string frame;
    for (int i = 0; i < 1024; ++i) {
        if (i > 0) { frame += ','; }
        frame += "255";
    }

    int accepted = 0;
    int refused = 0;
    for (int n = 0; n < 120; ++n) {
        const std::string upload = std::string(R"({"id":"i)") + std::to_string(n) +
                                   R"(","width":32,"height":32,"frames":[[)" + frame + "]]}";
        const Response response = fixture.call("POST", "/api/v1/assets", upload);
        if (response.status == 201) {
            ++accepted;
        } else {
            ++refused;
            // 3 KB each against a 256 KB budget: it runs out of room, not of
            // slots, and says so.
            STIPPLE_CHECK_EQ(response.status, 409);
            break;
        }
    }

    STIPPLE_CHECK(accepted > 64);   // more than the old cap allowed at any size
    STIPPLE_CHECK_EQ(refused, 1);   // and it did stop
}

STIPPLE_TEST(Assets, TheCollectionStaysMetadataOnly) {
    // Sixty-four icons' worth of pixels would be several hundred kilobytes of
    // JSON, built whole in RAM before a byte of it can be sent.
    Fixture fixture;
    fixture.call("POST", "/api/v1/assets",
                 R"({"id":"quad","width":2,"height":2,"frames":[[1,2,3,4]]})");

    const Response all = fixture.call("GET", "/api/v1/assets");
    STIPPLE_CHECK_EQ(all.status, 200);
    STIPPLE_CHECK(all.body.find("\"quad\"") != std::string::npos);
    STIPPLE_CHECK(all.body.find("\"pixels\"") == std::string::npos);
}

// --- the speaker -------------------------------------------------------------
//
// Until this route existed, nothing but a notification could make the device
// make a noise. These hold down the two things that matter about it: it plays
// only sounds the device can actually play, and it says so rather than
// pretending when there is no speaker behind it (ADR 0013).

STIPPLE_TEST(Api, TheSoundRouteIsRouted) {
    STIPPLE_CHECK(matchRoute("/api/v1/sound").resource == Resource::Sound);
}

STIPPLE_TEST(Api, ANamedSoundReachesTheSpeaker) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"sound":"chime"})").status, 204);

    const auto& played = fixture.platform.simulatedAudio().requests();
    STIPPLE_REQUIRE(played.size() == std::size_t{1});
    STIPPLE_CHECK_FALSE(played[0].isTone);
    STIPPLE_CHECK(played[0].sound == "chime");
}

STIPPLE_TEST(Api, AnUnknownSoundIs422AndPlaysNothing) {
    // 422 rather than 404: the route exists and the JSON was fine, the name
    // just is not one this device can make. Substituting a beep would be the
    // lie the catalogue exists to prevent.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"sound":"trumpet"})").status, 422);
    STIPPLE_CHECK(fixture.platform.simulatedAudio().requests().empty());
}

STIPPLE_TEST(Api, AnInlineToneIsPlayedAndBounded) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound",
                     R"({"frequencyHz":880,"durationMillis":200})").status, 204);

    const auto& played = fixture.platform.simulatedAudio().requests();
    STIPPLE_REQUIRE(played.size() == std::size_t{1});
    STIPPLE_CHECK(played[0].isTone);
    STIPPLE_CHECK_EQ(played[0].frequencyHz, 880);
    STIPPLE_CHECK_EQ(played[0].durationMillis, 200);
}

STIPPLE_TEST(Api, AToneOutsideTheLimitsIsRefused) {
    // The same ceilings a script gets. The network has no business reaching
    // further into the speaker than the device's own code does.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"frequencyHz":1})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"frequencyHz":40000})").status, 422);
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound",
                     R"({"frequencyHz":880,"durationMillis":60000})").status, 422);
    STIPPLE_CHECK(fixture.platform.simulatedAudio().requests().empty());
}

STIPPLE_TEST(Api, ARequestWithNothingToPlayIsABadRequest) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("POST", "/api/v1/sound", "{}").status, 400);
}

STIPPLE_TEST(Api, StopIsItsOwnRequest) {
    // "stop" is not a sound, and a caller asking for silence should not have
    // to know that the catalogue happens not to contain one.
    Fixture fixture;
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"stop":true})").status, 204);
    STIPPLE_CHECK_EQ(fixture.platform.simulatedAudio().stopCount(), std::uint32_t{1});
}

STIPPLE_TEST(Api, GettingTheSoundRouteListsWhatThisDeviceCanPlay) {
    // So a UI can offer the catalogue instead of hard-coding it, which is how
    // the web page's dropdown came to list three of the five sounds that
    // existed.
    Fixture fixture;
    const Response response = fixture.call("GET", "/api/v1/sound");
    STIPPLE_CHECK_EQ(response.status, 200);

    for (const char* name : {"beep", "chime", "alert", "tick", "tock"}) {
        STIPPLE_CHECK(response.body.find(std::string("\"") + name + "\"") !=
                      std::string::npos);
    }
    STIPPLE_CHECK(response.body.find("durationMillis") != std::string::npos);
    // Silence is a valid setting and not a sound, so a dropdown needs it named
    // separately rather than inferred.
    STIPPLE_CHECK(response.body.find("\"none\"") != std::string::npos);
}

STIPPLE_TEST(Api, ADeviceWithNoSpeakerSaysSoRatherThanAccepting) {
    stipple::platform::simulator::SimulatorCapabilities mute;
    mute.audio = false;
    Fixture fixture(mute, ApiOptions{});

    // 404 on both verbs: there is nothing here on this device, which is a
    // different answer from "your request was wrong".
    STIPPLE_CHECK_EQ(
        fixture.call("POST", "/api/v1/sound", R"({"sound":"chime"})").status, 404);
    STIPPLE_CHECK_EQ(fixture.call("GET", "/api/v1/sound").status, 404);
}

STIPPLE_TEST(Api, TheSoundRouteRejectsOtherVerbs) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("DELETE", "/api/v1/sound").status, 405);
}
