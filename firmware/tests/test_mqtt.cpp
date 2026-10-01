// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/mqtt/MqttService.h"

#include <string>

#include "stipple/api/ApiServer.h"
#include "stipple/app/AppRegistry.h"
#include "stipple/app/Carousel.h"
#include "stipple/config/Config.h"
#include "stipple/core/Log.h"
#include "stipple/mqtt/MqttBridge.h"
#include "stipple/notify/Notifications.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::api::ApiContext;
using stipple::api::ApiServer;
using stipple::config::Config;
using stipple::mqtt::Backoff;
using stipple::mqtt::Bridge;
using stipple::mqtt::MqttService;
using stipple::mqtt::ServiceContext;
using stipple::mqtt::Topics;
using stipple::platform::MqttMessage;
using stipple::platform::MqttState;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

int stateCode(MqttState state) { return static_cast<int>(state); }

/// A device with MQTT enabled, wired the way ApplicationHost wires it.
struct Fixture {
    SimulatorPlatform platform;
    stipple::app::AppRegistry apps;
    stipple::app::Carousel carousel{apps};
    stipple::notify::NotificationQueue notifications;
    stipple::log::RingLog logger;
    Config config;
    stipple::config::ConfigStore configStore{platform.storage()};
    ApiServer api;
    MqttService service;

    Fixture() : api(makeContext()) {
        config.mqtt.enabled = true;
        config.mqtt.host = "broker.local";
        config.deviceName = "stipple";

        ServiceContext context;
        context.client = platform.mqtt();
        context.api = &api;
        context.settings = &config;
        context.logger = &logger;
        service.setContext(context);
    }

    ApiContext makeContext() {
        ApiContext context;
        context.apps = &apps;
        context.carousel = &carousel;
        context.notifications = &notifications;
        context.config = &config;
        context.configStore = &configStore;
        context.platform = &platform;
        context.logger = &logger;
        return context;
    }

    /// Bring the connection up, as the host's loop would.
    void connect(std::uint64_t nowMillis = 0) {
        service.configure();
        service.tick(nowMillis);
    }

    stipple::platform::simulator::SimulatorMqtt& broker() { return platform.simulatedMqtt(); }

    /// Deliver a command and let the service answer it.
    void command(const std::string& suffix, const std::string& payload) {
        MqttMessage message;
        message.topic = service.topics().base + "/cmd/" + suffix;
        message.payload = payload;
        broker().deliver(message);
    }

    std::string payloadOn(const std::string& topic) {
        const MqttMessage* message = broker().lastOn(topic);
        return message != nullptr ? message->payload : std::string();
    }
};

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// --- backoff -----------------------------------------------------------------

STIPPLE_TEST(MqttBackoff, GrowsThenSettlesAtTheCeiling) {
    Backoff::Config config;
    config.firstDelayMillis = 100;
    config.maxDelayMillis = 800;
    Backoff backoff(config);

    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 100);
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 200);
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 400);
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 800);
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 800);
}

STIPPLE_TEST(MqttBackoff, ALongOutageNeverWrapsBackToZero) {
    // The failure this guards: an overflowing multiply turns backoff into a busy
    // loop, which is worst exactly when the network is already in trouble.
    Backoff backoff;
    std::uint32_t previous = 0;

    for (int i = 0; i < 2000; ++i) {
        const std::uint32_t delay = backoff.nextDelayMillis();
        STIPPLE_CHECK(delay >= previous || delay == 60000);
        STIPPLE_CHECK(delay > 0);
        STIPPLE_CHECK(delay <= 60000);
        previous = delay;
    }
    STIPPLE_CHECK_EQ(static_cast<int>(previous), 60000);
}

STIPPLE_TEST(MqttBackoff, SuccessStartsTheNextOutageShortAgain) {
    Backoff backoff;
    backoff.nextDelayMillis();
    backoff.nextDelayMillis();
    backoff.nextDelayMillis();

    backoff.reset();
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.currentDelayMillis()), 0);
    STIPPLE_CHECK_EQ(static_cast<int>(backoff.nextDelayMillis()), 1000);
}

// --- identifiers and topics --------------------------------------------------

STIPPLE_TEST(MqttTopics, DeviceIdIsSafeToPutInATopic) {
    using stipple::mqtt::deviceIdFromName;

    STIPPLE_CHECK_EQ(deviceIdFromName("stipple"), std::string("stipple"));
    STIPPLE_CHECK_EQ(deviceIdFromName("Kitchen Clock"), std::string("kitchen-clock"));
    STIPPLE_CHECK_EQ(deviceIdFromName("Kitchen -- 2"), std::string("kitchen-2"));
    STIPPLE_CHECK_EQ(deviceIdFromName("  spaced  "), std::string("spaced"));

    // Wildcards and separators would make the topic unsubscribable or ambiguous.
    STIPPLE_CHECK_EQ(deviceIdFromName("a/b#c+d"), std::string("a-b-c-d"));

    // A name with nothing usable in it still has to produce a topic segment.
    STIPPLE_CHECK_EQ(deviceIdFromName("###"), std::string("device"));
    STIPPLE_CHECK_EQ(deviceIdFromName(""), std::string("device"));
}

STIPPLE_TEST(MqttTopics, NamespaceFollowsTheBlueprint) {
    const Topics topics = Topics::build("stipple", "abc123");

    STIPPLE_CHECK_EQ(topics.base, std::string("stipple/abc123"));
    STIPPLE_CHECK_EQ(topics.availability, std::string("stipple/abc123/availability"));
    STIPPLE_CHECK_EQ(topics.status, std::string("stipple/abc123/status"));
    STIPPLE_CHECK_EQ(topics.button, std::string("stipple/abc123/button"));
    STIPPLE_CHECK_EQ(topics.commandFilter, std::string("stipple/abc123/cmd/#"));
}

STIPPLE_TEST(MqttTopics, AnEmptyBaseFallsBackRatherThanLeadingWithASlash) {
    STIPPLE_CHECK_EQ(Topics::build("", "abc").base, std::string("stipple/abc"));
}

// --- translation -------------------------------------------------------------

STIPPLE_TEST(MqttBridge, CommandsBecomeApiCalls) {
    Bridge bridge;
    bridge.setTopics(Topics::build("stipple", "abc"));

    const auto translate = [&bridge](const char* topic, const char* payload) {
        MqttMessage message;
        message.topic = topic;
        message.payload = payload;
        return bridge.translate(message);
    };

    auto notify = translate("stipple/abc/cmd/notify", R"({"text":"hi"})");
    STIPPLE_CHECK(notify.understood);
    STIPPLE_CHECK_EQ(notify.request.path, std::string("/api/v1/notifications"));
    STIPPLE_CHECK(notify.request.method == stipple::api::Method::Post);
    STIPPLE_CHECK_EQ(notify.request.body, std::string(R"({"text":"hi"})"));

    auto settings = translate("stipple/abc/cmd/settings", R"({"display":{"brightness":10}})");
    STIPPLE_CHECK(settings.understood);
    STIPPLE_CHECK_EQ(settings.request.path, std::string("/api/v1/settings"));
    STIPPLE_CHECK(settings.request.method == stipple::api::Method::Patch);

    auto activate = translate("stipple/abc/cmd/apps/clock/activate", "");
    STIPPLE_CHECK(activate.understood);
    STIPPLE_CHECK_EQ(activate.request.path, std::string("/api/v1/apps/clock/activate"));
    STIPPLE_CHECK(activate.request.method == stipple::api::Method::Post);

    auto input = translate("stipple/abc/cmd/input", R"({"control":"plus"})");
    STIPPLE_CHECK(input.understood);
    STIPPLE_CHECK_EQ(input.request.path, std::string("/api/v1/input"));
    STIPPLE_CHECK(input.request.method == stipple::api::Method::Post);
    STIPPLE_CHECK_EQ(input.request.body, std::string(R"({"control":"plus"})"));

    // The payload passes through untouched, so the API is the only thing that
    // decides whether a sound name is real - two paths validating the same
    // name separately is how they come to disagree.
    auto sound = translate("stipple/abc/cmd/sound", R"({"sound":"chime"})");
    STIPPLE_CHECK(sound.understood);
    STIPPLE_CHECK_EQ(sound.request.path, std::string("/api/v1/sound"));
    STIPPLE_CHECK(sound.request.method == stipple::api::Method::Post);
    STIPPLE_CHECK_EQ(sound.request.body, std::string(R"({"sound":"chime"})"));

    auto reboot = translate("stipple/abc/cmd/reboot", "");
    STIPPLE_CHECK(reboot.understood);
    STIPPLE_CHECK_EQ(reboot.request.path, std::string("/api/v1/system/reboot"));
}

STIPPLE_TEST(MqttBridge, AnEmptyRetainedAppMeansDelete) {
    // Publishing an empty retained message is how MQTT conventionally says "this
    // is gone", so a retained app can be cleared the way the ecosystem expects.
    Bridge bridge;
    bridge.setTopics(Topics::build("stipple", "abc"));

    MqttMessage message;
    message.topic = "stipple/abc/cmd/apps/weather";

    message.payload = "";
    STIPPLE_CHECK(bridge.translate(message).request.method == stipple::api::Method::Delete);

    message.payload = R"({"enabled":false})";
    STIPPLE_CHECK(bridge.translate(message).request.method == stipple::api::Method::Patch);
}

STIPPLE_TEST(MqttBridge, UnknownTopicsAreNotGuessedAt) {
    Bridge bridge;
    bridge.setTopics(Topics::build("stipple", "abc"));

    const char* rejected[] = {
        "stipple/abc/cmd/explode",          // no such command
        "stipple/abc/cmd/apps/clock/spin",  // no such action
        "stipple/abc/cmd/",                 // nothing after the prefix
        "stipple/abc/status",               // ours, but not a command
        "stipple/other/cmd/notify",         // another device
        "somewhere/else/cmd/notify",
        "",
    };

    for (const char* topic : rejected) {
        MqttMessage message;
        message.topic = topic;
        STIPPLE_CHECK_FALSE(bridge.translate(message).understood);
    }
}

STIPPLE_TEST(MqttBridge, RepliesLandOutsideTheCommandSubscription) {
    // Answering inside /cmd/# would echo every reply back to this device, which
    // would then report it as an unknown command — a loop that only appears once
    // a real broker is attached.
    Bridge bridge;
    bridge.setTopics(Topics::build("stipple", "abc"));

    MqttMessage message;
    message.topic = "stipple/abc/cmd/notify";
    const auto translation = bridge.translate(message);

    STIPPLE_CHECK(translation.understood);
    STIPPLE_CHECK_EQ(translation.replyTopic, std::string("stipple/abc/result/notify"));
    STIPPLE_CHECK(translation.replyTopic.find("/cmd/") == std::string::npos);
}

STIPPLE_TEST(MqttBridge, ConnectOptionsCarryAWill) {
    Config config;
    config.deviceName = "Kitchen Clock";
    config.mqtt.host = "broker.local";

    const Topics topics = Topics::build("stipple", "kitchen-clock");
    const auto options = Bridge::connectOptions(config, topics);

    // Availability that depends on the device being well enough to announce its
    // own death is not availability.
    STIPPLE_CHECK_EQ(options.willTopic, topics.availability);
    STIPPLE_CHECK_EQ(options.willPayload, std::string("offline"));
    STIPPLE_CHECK(options.willRetained);

    // A client id that changes every boot leaves stale sessions on the broker.
    STIPPLE_CHECK_EQ(options.clientId, std::string("stipple-kitchen-clock"));
}

// --- the service -------------------------------------------------------------

STIPPLE_TEST(MqttService, DisabledByDefaultAndNeverDialsOut) {
    // §20: a device must be fully usable without a broker, and must never talk
    // to one nobody asked it to.
    Fixture fixture;
    fixture.config.mqtt.enabled = false;

    fixture.service.configure();
    fixture.service.tick(0);

    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Disabled));
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 0);
    STIPPLE_CHECK(fixture.broker().published().empty());
}

STIPPLE_TEST(MqttService, EnabledWithNoHostStaysOff) {
    Fixture fixture;
    fixture.config.mqtt.host.clear();

    fixture.connect();
    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Disabled));
}

STIPPLE_TEST(MqttService, AnnouncesItselfOnConnect) {
    Fixture fixture;
    fixture.connect();

    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Connected));

    const MqttMessage* availability = fixture.broker().lastOn(fixture.service.topics().availability);
    STIPPLE_REQUIRE(availability != nullptr);
    STIPPLE_CHECK_EQ(availability->payload, std::string("online"));
    // Retained, so a subscriber arriving later still learns the device is up.
    STIPPLE_CHECK(availability->retained);

    // And it listens for commands.
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.broker().subscriptions().size()), 1);
    STIPPLE_CHECK_EQ(fixture.broker().subscriptions()[0], fixture.service.topics().commandFilter);
}

STIPPLE_TEST(MqttService, PublishesRetainedStatus) {
    Fixture fixture;
    fixture.config.display.brightness = 200;

    MqttService::DeviceState state;
    state.activeAppId = "clock";
    state.healthy = true;
    fixture.service.setDeviceState(state);
    fixture.connect();

    const MqttMessage* status = fixture.broker().lastOn(fixture.service.topics().status);
    STIPPLE_REQUIRE(status != nullptr);
    STIPPLE_CHECK(status->retained);
    STIPPLE_CHECK(contains(status->payload, "\"activeApp\":\"clock\""));
    STIPPLE_CHECK(contains(status->payload, "\"brightness\":200"));
    STIPPLE_CHECK(contains(status->payload, "\"online\":true"));
}

STIPPLE_TEST(MqttService, SayingGoodbyeDoesNotWaitForAKeepalive) {
    Fixture fixture;
    fixture.connect();
    fixture.broker().clear();

    fixture.service.shutdown();

    const MqttMessage* availability = fixture.broker().lastOn(fixture.service.topics().availability);
    STIPPLE_REQUIRE(availability != nullptr);
    STIPPLE_CHECK_EQ(availability->payload, std::string("offline"));
    STIPPLE_CHECK(availability->retained);
}

STIPPLE_TEST(MqttService, RetriesAnUnreachableBrokerWithBackoff) {
    Fixture fixture;
    fixture.broker().setReachable(false);

    fixture.service.configure();

    fixture.service.tick(0);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 1);

    // Too soon: the whole point is not to hammer a broker that is down.
    fixture.service.tick(500);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 1);

    fixture.service.tick(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 2);

    // Second wait is longer than the first.
    fixture.service.tick(2000);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 2);
    fixture.service.tick(3000);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts), 3);
}

STIPPLE_TEST(MqttService, ABrokerThatComesBackIsPickedUp) {
    Fixture fixture;
    fixture.broker().setReachable(false);
    fixture.service.configure();
    fixture.service.tick(0);
    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Disconnected));

    fixture.broker().setReachable(true);
    for (std::uint64_t now = 1000; now <= 20000; now += 1000) {
        fixture.service.tick(now);
    }

    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Connected));
    STIPPLE_CHECK_EQ(fixture.payloadOn(fixture.service.topics().availability),
                    std::string("online"));
}

STIPPLE_TEST(MqttService, PublishFailuresAreCountedNotQueued) {
    // §38 forbids an unbounded queue, so a full transport must be allowed to say
    // no and the caller must notice.
    Fixture fixture;
    fixture.connect();

    fixture.broker().setPublishAccepted(false);
    const std::uint32_t before = fixture.service.stats().publishFailures;

    fixture.service.invalidateStatus();
    fixture.service.tick(1000);

    STIPPLE_CHECK(fixture.service.stats().publishFailures > before);
}

// --- MQTT drives the device --------------------------------------------------

STIPPLE_TEST(MqttService, ACommandActuallyChangesTheDevice) {
    // The exit criterion for this phase: usable from MQTT without HTTP.
    Fixture fixture;
    fixture.connect();

    fixture.command("settings", R"({"display":{"brightness":42}})");

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.config.display.brightness), 42);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().commandsHandled), 1);
}

STIPPLE_TEST(MqttService, CommandsAreValidatedExactlyLikeHttp) {
    // Translating into an api::Request rather than reimplementing the handlers
    // is what makes this true by construction.
    Fixture fixture;
    fixture.connect();
    fixture.broker().clear();

    fixture.command("settings", R"({"clock":{"theme":"holographic"}})");

    STIPPLE_CHECK_EQ(fixture.config.clock.theme, std::string("minimal"));

    const std::string reply = fixture.payloadOn(fixture.service.topics().base +
                                                "/result/settings");
    STIPPLE_CHECK(contains(reply, "\"status\":422"));
}

STIPPLE_TEST(MqttService, SuccessAndFailureAreBothAnswered) {
    Fixture fixture;
    fixture.connect();
    fixture.broker().clear();

    fixture.command("notify", R"({"text":"Dinner"})");

    const MqttMessage* reply =
        fixture.broker().lastOn(fixture.service.topics().base + "/result/notify");
    STIPPLE_REQUIRE(reply != nullptr);
    STIPPLE_CHECK(contains(reply->payload, "\"status\":201"));
    // A stale success from last week would be actively misleading.
    STIPPLE_CHECK_FALSE(reply->retained);
}

STIPPLE_TEST(MqttService, UnknownCommandsAreReportedNotSilentlyDropped) {
    Fixture fixture;
    fixture.connect();

    fixture.command("explode", "{}");

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().commandsRejected), 1);
    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().commandsHandled), 0);

    bool logged = false;
    for (int i = 0; i < fixture.logger.count(); ++i) {
        if (std::string(fixture.logger.at(i).message).find("unknown command") !=
            std::string::npos) {
            logged = true;
        }
    }
    STIPPLE_CHECK(logged);
}

STIPPLE_TEST(MqttService, AChangeMadeOverMqttIsRepublished) {
    Fixture fixture;
    fixture.connect();
    fixture.broker().clear();

    fixture.command("settings", R"({"display":{"brightness":77}})");
    fixture.service.tick(1000);

    STIPPLE_CHECK(contains(fixture.payloadOn(fixture.service.topics().status),
                          "\"brightness\":77"));
}

// --- secrets -----------------------------------------------------------------

STIPPLE_TEST(MqttService, TheBrokerPasswordNeverLeavesTheDevice) {
    // §22. The one place the password may appear is the connect options; a
    // broker republishing retained state is the last place it should surface.
    Fixture fixture;
    fixture.config.mqtt.username = "stipple";
    fixture.config.mqtt.password = "hunter2-do-not-leak";

    MqttService::DeviceState state;
    state.activeAppId = "clock";
    fixture.service.setDeviceState(state);

    fixture.connect();
    fixture.command("settings", R"({"display":{"brightness":5}})");
    fixture.service.tick(1000);

    // It did reach the transport, or the device could not reconnect.
    STIPPLE_CHECK_EQ(fixture.broker().lastConnectOptions().password,
                    std::string("hunter2-do-not-leak"));

    for (const MqttMessage& message : fixture.broker().published()) {
        STIPPLE_CHECK_FALSE(contains(message.payload, "hunter2-do-not-leak"));
    }
    for (int i = 0; i < fixture.logger.count(); ++i) {
        STIPPLE_CHECK_FALSE(
            contains(std::string(fixture.logger.at(i).message), "hunter2-do-not-leak"));
    }
}

STIPPLE_TEST(MqttService, ReconfiguringPointsAtTheNewBroker) {
    Fixture fixture;
    fixture.connect();
    STIPPLE_CHECK_EQ(fixture.broker().lastConnectOptions().host, std::string("broker.local"));

    fixture.config.mqtt.host = "other.local";
    fixture.service.configure();
    fixture.service.tick(10000);

    STIPPLE_CHECK_EQ(fixture.broker().lastConnectOptions().host, std::string("other.local"));
}

STIPPLE_TEST(MqttService, ReconfiguringWithNoChangeDoesNotChurnTheConnection) {
    Fixture fixture;
    fixture.connect();
    const std::uint32_t attempts = fixture.service.stats().connectAttempts;

    for (int i = 0; i < 5; ++i) {
        fixture.service.configure();
        fixture.service.tick(static_cast<std::uint64_t>(i + 1) * 1000u);
    }

    STIPPLE_CHECK_EQ(static_cast<int>(fixture.service.stats().connectAttempts),
                    static_cast<int>(attempts));
    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Connected));
}

STIPPLE_TEST(MqttService, SwitchingMqttOffDisconnects) {
    Fixture fixture;
    fixture.connect();
    fixture.broker().clear();

    fixture.config.mqtt.enabled = false;
    fixture.service.configure();

    STIPPLE_CHECK_EQ(stateCode(fixture.service.state()), stateCode(MqttState::Disabled));
    STIPPLE_CHECK_EQ(fixture.payloadOn(fixture.service.topics().availability),
                    std::string("offline"));
}

// --- button events -----------------------------------------------------------

STIPPLE_TEST(MqttService, ButtonEventsArePublishedButNotRetained) {
    Fixture fixture;
    fixture.connect();

    fixture.service.publishButton("appNext", 3, false);

    const MqttMessage* event = fixture.broker().lastOn(fixture.service.topics().button);
    STIPPLE_REQUIRE(event != nullptr);
    STIPPLE_CHECK(contains(event->payload, "\"action\":\"appNext\""));
    STIPPLE_CHECK(contains(event->payload, "\"repeat\":3"));
    // A retained button press would replay every time something subscribed.
    STIPPLE_CHECK_FALSE(event->retained);
}

STIPPLE_TEST(MqttService, ButtonEventsWhileDisconnectedAreDroppedNotBuffered) {
    Fixture fixture;
    fixture.broker().setReachable(false);
    fixture.service.configure();
    fixture.service.tick(0);

    fixture.service.publishButton("appNext", 1, false);

    STIPPLE_CHECK(fixture.broker().published().empty());
}
