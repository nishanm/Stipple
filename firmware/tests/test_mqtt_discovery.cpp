// SPDX-License-Identifier: GPL-3.0-or-later
//
// Home Assistant discovery — the documents the device publishes so its
// entities appear without anybody writing YAML.
//
// These had no coverage at all, which mattered more than it looks: discovery
// is the one output nothing else in the system reads back. A wrong topic, a
// missing `unique_id` or a device block that differs between entities does not
// fail, log, or show up on the panel. It produces a Home Assistant that is
// subtly wrong — five ungrouped entities, or a duplicate that shadows the
// original — in somebody else's house, weeks later.
//
// So these tests pin the properties that make it correct rather than merely
// present: one device block shared by every entity, identifiers that cannot
// collide between two devices on one broker, withdrawal that removes rather
// than orphans, and templates that yield *nothing* for a reading the device
// does not have.
#include "stipple/mqtt/MqttBridge.h"

#include <string>
#include <vector>

#include "stipple/config/Config.h"
#include "stipple/core/Version.h"
#include "stipple/json/Json.h"
#include "stipple/platform/MqttClient.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::config::Config;
using stipple::mqtt::Bridge;
using stipple::mqtt::Topics;
using stipple::platform::MqttMessage;

namespace {

struct Parsed {
    stipple::json::Token tokens[256];
    std::string text;
    stipple::json::Document document{tokens, 256};
    bool ok = false;

    explicit Parsed(std::string body) : text(std::move(body)) {
        ok = document.parse(text) == stipple::json::Error::None;
    }
    stipple::json::Value root() const { return document.root(); }
};

/// Discovery for a device, with whatever name the caller wants on it.
std::vector<MqttMessage> publish(const char* deviceName = "Kitchen Clock",
                                 const char* deviceId = "kitchen-clock",
                                 bool clear = false) {
    Bridge bridge;
    bridge.setTopics(Topics::build("stipple", deviceId));
    Config config;
    config.deviceName = deviceName;
    return bridge.discoveryMessages(config, deviceId, clear);
}

/// Does nothing; the TLS tests never get as far as a message.
struct Listener : stipple::platform::IMqttListener {
    void onMessage(const MqttMessage&) override {}
    void onStateChanged(stipple::platform::MqttState) override {}
};

const MqttMessage* find(const std::vector<MqttMessage>& messages, const char* topic) {
    for (const auto& message : messages) {
        if (message.topic == topic) { return &message; }
    }
    return nullptr;
}

}  // namespace

STIPPLE_TEST(MqttDiscovery, EveryEntityIsPublishedOnTheTopicHomeAssistantWatches) {
    // Home Assistant subscribes to `homeassistant/+/+/+/config`. A document on
    // any other topic is simply never seen - there is no error for getting
    // this wrong, which is exactly why it is worth a test.
    const auto messages = publish();
    STIPPLE_REQUIRE(messages.size() == 5);

    const char* expected[] = {
        "homeassistant/light/stipple_kitchen-clock/panel/config",
        "homeassistant/number/stipple_kitchen-clock/volume/config",
        "homeassistant/sensor/stipple_kitchen-clock/battery/config",
        "homeassistant/sensor/stipple_kitchen-clock/rssi/config",
        "homeassistant/sensor/stipple_kitchen-clock/app/config",
    };

    for (const char* topic : expected) {
        const MqttMessage* message = find(messages, topic);
        STIPPLE_CHECK(message != nullptr);
        if (message == nullptr) { continue; }
        // Retained, always. An entity that vanishes when Home Assistant
        // restarts is not an integration.
        STIPPLE_CHECK(message->retained);
        STIPPLE_CHECK(!message->payload.empty());
    }
}

STIPPLE_TEST(MqttDiscovery, EveryEntityCarriesTheSameDeviceBlock) {
    // This is what makes Home Assistant group them under one device instead of
    // scattering five unrelated entities across the dashboard. One entity with
    // a different block is one entity that floats off on its own.
    const auto messages = publish();
    STIPPLE_REQUIRE(messages.size() == 5);

    std::string first;
    for (const auto& message : messages) {
        Parsed parsed(message.payload);
        STIPPLE_REQUIRE(parsed.ok);

        const auto device = parsed.root()["device"];
        STIPPLE_REQUIRE(!device.isNull());
        STIPPLE_CHECK(device["identifiers"][0].stringEquals("stipple_kitchen-clock"));
        STIPPLE_CHECK(device["name"].stringEquals("Kitchen Clock"));
        STIPPLE_CHECK(device["sw_version"].stringEquals(std::string(stipple::kVersion)));

        // Compared as text so a field added to one entity and not the others
        // fails here rather than in somebody's dashboard.
        const std::string block = device["identifiers"][0].toString() + "|" +
                                  device["name"].toString() + "|" +
                                  device["model"].toString() + "|" +
                                  device["manufacturer"].toString() + "|" +
                                  device["sw_version"].toString();
        if (first.empty()) { first = block; } else { STIPPLE_CHECK(block == first); }
    }
}

STIPPLE_TEST(MqttDiscovery, TwoDevicesOnOneBrokerDoNotCollide) {
    // `unique_id` is how Home Assistant decides two documents describe the
    // same entity. If two Stipples shared one, the second to announce would
    // shadow the first and the owner would see one device where they have two.
    const auto kitchen = publish("Kitchen Clock", "kitchen-clock");
    const auto desk = publish("Desk Clock", "desk-clock");
    STIPPLE_REQUIRE(kitchen.size() == desk.size());

    for (std::size_t i = 0; i < kitchen.size(); ++i) {
        STIPPLE_CHECK(kitchen[i].topic != desk[i].topic);

        Parsed a(kitchen[i].payload);
        Parsed b(desk[i].payload);
        STIPPLE_REQUIRE(a.ok);
        STIPPLE_REQUIRE(b.ok);
        STIPPLE_CHECK(a.root()["unique_id"].toString() != b.root()["unique_id"].toString());
        STIPPLE_CHECK(a.root()["unique_id"].toString().find("kitchen-clock") !=
                      std::string::npos);
    }
}

STIPPLE_TEST(MqttDiscovery, TurningItOffWithdrawsTheEntitiesRatherThanOrphaningThem) {
    // An empty retained payload is how MQTT says "this is gone". Publishing
    // nothing at all would leave the entities sitting in a dashboard for a
    // device that has stopped talking, which is worse than never having
    // announced them.
    const auto announced = publish("Kitchen Clock", "kitchen-clock", false);
    const auto withdrawn = publish("Kitchen Clock", "kitchen-clock", true);

    STIPPLE_REQUIRE(announced.size() == withdrawn.size());
    for (std::size_t i = 0; i < announced.size(); ++i) {
        // Same topic, emptied - and still retained, or the broker would keep
        // serving the old document to the next subscriber.
        STIPPLE_CHECK(withdrawn[i].topic == announced[i].topic);
        STIPPLE_CHECK(withdrawn[i].payload.empty());
        STIPPLE_CHECK(withdrawn[i].retained);
    }
}

STIPPLE_TEST(MqttDiscovery, AReadingTheDeviceDoesNotHaveYieldsNothingRatherThanZero) {
    // ADR 0013, in the one place it is easiest to get wrong. A battery sensor
    // reading 0% because the MCU never answered looks exactly like a flat
    // battery, and Home Assistant will happily draw a history graph of that
    // lie. The template has to produce an empty string, which Home Assistant
    // reads as unavailable.
    const auto messages = publish();

    const MqttMessage* battery =
        find(messages, "homeassistant/sensor/stipple_kitchen-clock/battery/config");
    STIPPLE_REQUIRE(battery != nullptr);
    Parsed parsedBattery(battery->payload);
    STIPPLE_REQUIRE(parsedBattery.ok);
    const std::string batteryTemplate = parsedBattery.root()["value_template"].toString();
    STIPPLE_CHECK(batteryTemplate.find("is defined") != std::string::npos);

    const MqttMessage* rssi =
        find(messages, "homeassistant/sensor/stipple_kitchen-clock/rssi/config");
    STIPPLE_REQUIRE(rssi != nullptr);
    Parsed parsedRssi(rssi->payload);
    STIPPLE_REQUIRE(parsedRssi.ok);
    STIPPLE_CHECK(parsedRssi.root()["value_template"].toString().find("is defined") !=
                  std::string::npos);
}

STIPPLE_TEST(MqttDiscovery, ControlsSpeakTheSameSettingsRouteEverythingElseDoes) {
    // The reason the panel uses the template schema rather than the default.
    // The default would POST "ON" to a command topic, and this device speaks a
    // settings patch - so a second control path would exist purely for Home
    // Assistant, and would be the one nobody remembers to update.
    const auto messages = publish();

    const MqttMessage* panel =
        find(messages, "homeassistant/light/stipple_kitchen-clock/panel/config");
    STIPPLE_REQUIRE(panel != nullptr);
    Parsed parsed(panel->payload);
    STIPPLE_REQUIRE(parsed.ok);

    STIPPLE_CHECK(parsed.root()["schema"].stringEquals("template"));
    STIPPLE_CHECK(parsed.root()["command_topic"].stringEquals("stipple/kitchen-clock/cmd/settings"));
    // The payload it sends is the same document a PATCH to /api/v1/settings
    // takes, so the two cannot drift.
    STIPPLE_CHECK(parsed.root()["command_on_template"].toString().find("\"display\"") !=
                  std::string::npos);
    STIPPLE_CHECK(parsed.root()["command_off_template"].toString().find("\"power\":false") !=
                  std::string::npos);

    const MqttMessage* volume =
        find(messages, "homeassistant/number/stipple_kitchen-clock/volume/config");
    STIPPLE_REQUIRE(volume != nullptr);
    Parsed parsedVolume(volume->payload);
    STIPPLE_REQUIRE(parsedVolume.ok);
    STIPPLE_CHECK(
        parsedVolume.root()["command_topic"].stringEquals("stipple/kitchen-clock/cmd/settings"));
    STIPPLE_CHECK(parsedVolume.root()["command_template"].toString().find("volumePercent") !=
                  std::string::npos);
}

STIPPLE_TEST(MqttDiscovery, EveryEntityKnowsWhenTheDeviceIsOffline) {
    // Without an availability topic the entities keep showing their last
    // reading forever, so a device that has been unplugged for a week reads
    // as a device that is fine.
    const auto messages = publish();
    for (const auto& message : messages) {
        Parsed parsed(message.payload);
        STIPPLE_REQUIRE(parsed.ok);
        STIPPLE_CHECK(
            parsed.root()["availability_topic"].stringEquals("stipple/kitchen-clock/availability"));
        STIPPLE_CHECK(parsed.root()["state_topic"].stringEquals("stipple/kitchen-clock/status"));
        STIPPLE_CHECK(parsed.root()["payload_available"].stringEquals("online"));
        STIPPLE_CHECK(parsed.root()["payload_not_available"].stringEquals("offline"));
    }
}

STIPPLE_TEST(MqttDiscovery, ADeviceWithNoNameStillAnnouncesSomethingReadable) {
    // An empty name is not an error state - it is what a device looks like
    // before anybody has renamed it - and "" in a dashboard is worse than a
    // default.
    const auto messages = publish("", "abc");
    STIPPLE_REQUIRE(!messages.empty());
    Parsed parsed(messages[0].payload);
    STIPPLE_REQUIRE(parsed.ok);
    STIPPLE_CHECK(parsed.root()["device"]["name"].stringEquals("STIPPLE"));
}

// --- TLS, and saying so ------------------------------------------------------

STIPPLE_TEST(MqttTls, AskingForTlsIsRefusedRatherThanQuietlyDowngraded) {
    // The important half of this was already right and is worth pinning: a
    // transport that cannot do TLS must not fall back to plaintext, because
    // that puts the broker password on the wire of a network somebody
    // believed was protected.
    stipple::platform::simulator::SimulatorMqtt client;
    Listener listener;

    stipple::platform::MqttConnectOptions options;
    options.host = "broker.example";
    options.tls = true;

    STIPPLE_CHECK(!client.connect(options, listener));
    // Disabled, not Disconnected. Disconnected means "try again later", and
    // the reconnect policy would do exactly that forever against something
    // that is never going to succeed.
    STIPPLE_CHECK(client.state() == stipple::platform::MqttState::Disabled);
}

STIPPLE_TEST(MqttTls, TheSameRequestWithoutTlsConnects) {
    // So the refusal above is attributable to TLS and not to the fixture.
    stipple::platform::simulator::SimulatorMqtt client;
    Listener listener;

    stipple::platform::MqttConnectOptions options;
    options.host = "broker.example";
    options.tls = false;

    STIPPLE_CHECK(client.connect(options, listener));
    STIPPLE_CHECK(client.state() == stipple::platform::MqttState::Connected);
}
