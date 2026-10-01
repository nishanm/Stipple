// SPDX-License-Identifier: GPL-3.0-or-later
//
// The broker, as a script sees it.
//
// Three layers, tested separately because they fail separately: the topic
// rules (pure functions, and two of them are the security boundary), the
// gateway that caches and scopes, and the Berry builtins on top.
//
// The rule worth stating once here rather than in every test below: a script
// is untrusted code that arrived over the network. It may publish only under
// its own subtree, and it may read only what it asked for. Everything else in
// this file is a consequence of those two sentences.
#include "stipple/script/IScriptMqtt.h"

#include <cstdint>
#include <string>
#include <vector>

#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/mqtt/ScriptGateway.h"
#include "stipple/platform/MqttClient.h"
#include "stipple/script/ScriptStore.h"
#include "support/TestFramework.h"

using stipple::Canvas;
using stipple::Framebuffer;
using stipple::mqtt::ScriptGateway;
using stipple::platform::MqttMessage;
using stipple::script::IScriptMqtt;
using stipple::script::ScriptPutResult;
using stipple::script::ScriptStore;
using stipple::script::topicMatches;
using stipple::script::validPublishLeaf;
using stipple::script::validWatchFilter;
namespace colors = stipple::colors;

namespace {

/// A broker that records rather than connects.
class FakeClient final : public stipple::platform::IMqttClient {
public:
    std::vector<MqttMessage> published;
    std::vector<std::string> subscribed;
    bool accept = true;

    bool connect(const stipple::platform::MqttConnectOptions&,
                 stipple::platform::IMqttListener&) override {
        return true;
    }
    void disconnect() override {}
    stipple::platform::MqttState state() const override {
        return stipple::platform::MqttState::Connected;
    }
    bool publish(const MqttMessage& message) override {
        if (!accept) {
            return false;
        }
        published.push_back(message);
        return true;
    }
    bool subscribe(std::string_view filter, int) override {
        subscribed.emplace_back(filter);
        return true;
    }
    void poll(std::uint64_t) override {}
    bool supportsTls() const override { return false; }
};

/// A gateway wired to a fake client and already "connected".
struct Rig {
    FakeClient client;
    ScriptGateway gateway;

    Rig() {
        gateway.setClient(&client);
        gateway.setBase("stipple/kitchen");
        gateway.setConnected(true);
    }

    void arrive(const char* topic, const char* payload, std::uint64_t at = 0) {
        gateway.setNowMillis(at);
        MqttMessage message;
        message.topic = topic;
        message.payload = payload;
        gateway.deliver(message);
    }
};

int countLit(const Framebuffer& framebuffer) {
    int lit = 0;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (framebuffer.at(x, y) != colors::kBlack) {
                ++lit;
            }
        }
    }
    return lit;
}

}  // namespace

// --- the topic rules ---------------------------------------------------------

STIPPLE_TEST(MqttTopics, AWildcardIsNeverAValidThingToPublishTo) {
    // A script publishing to "#" is publishing to a filter, and what a broker
    // does with that is not something to discover on somebody's home network.
    STIPPLE_CHECK(!validPublishLeaf("#"));
    STIPPLE_CHECK(!validPublishLeaf("state/#"));
    STIPPLE_CHECK(!validPublishLeaf("+/state"));
    STIPPLE_CHECK(validPublishLeaf("state"));
    STIPPLE_CHECK(validPublishLeaf("sensor/temperature"));
}

STIPPLE_TEST(MqttTopics, AnEmptyLevelIsRefusedWhereverItAppears) {
    // Legal MQTT and always a mistake here: it means somebody concatenated a
    // prefix that already ended in a slash.
    STIPPLE_CHECK(!validPublishLeaf(""));
    STIPPLE_CHECK(!validPublishLeaf("/state"));
    STIPPLE_CHECK(!validPublishLeaf("state/"));
    STIPPLE_CHECK(!validPublishLeaf("a//b"));
}

STIPPLE_TEST(MqttTopics, AControlCharacterCannotReachTheBroker) {
    // Some brokers accept a topic with a newline in it and some truncate it,
    // and a script author would have no way to tell which had happened.
    STIPPLE_CHECK(!validPublishLeaf(std::string_view("sta\nte")));
    STIPPLE_CHECK(!validWatchFilter(std::string_view("home/\x01/x")));
}

STIPPLE_TEST(MqttTopics, AWildcardMustOccupyAWholeLevel) {
    // "sensor+/x" matches nothing, but a broker accepts the subscription
    // happily - so a script that wrote it would show nothing for ever with no
    // error anywhere. This is the only place anybody finds out.
    STIPPLE_CHECK(validWatchFilter("home/+/temperature"));
    STIPPLE_CHECK(validWatchFilter("home/#"));
    STIPPLE_CHECK(validWatchFilter("#"));
    STIPPLE_CHECK(!validWatchFilter("home/sensor+/x"));
    STIPPLE_CHECK(!validWatchFilter("home/#/x"));
    STIPPLE_CHECK(!validWatchFilter("home/a#"));
}

STIPPLE_TEST(MqttTopics, PlusMatchesExactlyOneLevel) {
    STIPPLE_CHECK(topicMatches("home/+/temp", "home/kitchen/temp"));
    STIPPLE_CHECK(!topicMatches("home/+/temp", "home/kitchen/left/temp"));
    STIPPLE_CHECK(!topicMatches("home/+/temp", "home/temp"));
    STIPPLE_CHECK(topicMatches("+/+", "a/b"));
}

STIPPLE_TEST(MqttTopics, HashMatchesTheRestIncludingNothing) {
    STIPPLE_CHECK(topicMatches("home/#", "home/kitchen/temp"));
    STIPPLE_CHECK(topicMatches("home/#", "home/temp"));
    // The parent itself matches its own multi-level wildcard. This is in the
    // spec and is the case every hand-rolled matcher gets wrong.
    STIPPLE_CHECK(topicMatches("home/#", "home"));
    STIPPLE_CHECK(!topicMatches("home/#", "garden/temp"));
}

STIPPLE_TEST(MqttTopics, AnExactFilterMatchesOnlyItself) {
    STIPPLE_CHECK(topicMatches("a/b/c", "a/b/c"));
    STIPPLE_CHECK(!topicMatches("a/b/c", "a/b"));
    STIPPLE_CHECK(!topicMatches("a/b", "a/b/c"));
    STIPPLE_CHECK(!topicMatches("a/b/c", "a/b/d"));
}

STIPPLE_TEST(MqttTopics, TheBrokersOwnTopicsAreNotCaughtByARootWildcard) {
    // $SYS is the broker talking about itself, and by convention a wildcard at
    // the root does not reach it. A script watching "#" that started receiving
    // the broker's internal load average would be surprising in a way nobody
    // would debug quickly.
    STIPPLE_CHECK(!topicMatches("#", "$SYS/broker/uptime"));
    STIPPLE_CHECK(!topicMatches("+/broker", "$SYS/broker"));
    STIPPLE_CHECK(topicMatches("$SYS/#", "$SYS/broker/uptime"));
}

// --- the gateway -------------------------------------------------------------

STIPPLE_TEST(ScriptMqtt, APublishLandsUnderTheScriptsOwnSubtree) {
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.publish("thermostat", "setpoint", "21.5", false));
    STIPPLE_REQUIRE(rig.client.published.size() == 1);
    STIPPLE_CHECK(rig.client.published[0].topic ==
                  "stipple/kitchen/script/thermostat/setpoint");
    STIPPLE_CHECK(rig.client.published[0].payload == "21.5");
    STIPPLE_CHECK(!rig.client.published[0].retained);
}

STIPPLE_TEST(ScriptMqtt, AScriptCannotEscapeItsOwnSubtree) {
    // The whole reason publishing is scoped. A script that could write to
    // ".../status" could tell somebody's dashboard the device was healthy
    // while it was on fire.
    Rig rig;
    STIPPLE_CHECK(!rig.gateway.publish("evil", "../../status", "online", true));
    STIPPLE_CHECK(!rig.gateway.publish("evil", "/status", "online", true));
    STIPPLE_CHECK(!rig.gateway.publish("evil", "#", "online", true));
    STIPPLE_CHECK(!rig.gateway.publish("evil", "..", "x", false));
    STIPPLE_CHECK(rig.client.published.empty());

    // The ordinary case still works, so the rule above is a rule and not a
    // ban on slashes.
    STIPPLE_REQUIRE(rig.gateway.publish("good", "sensor/kitchen/temp", "21", false));
    STIPPLE_CHECK(rig.client.published[0].topic ==
                  "stipple/kitchen/script/good/sensor/kitchen/temp");
}

STIPPLE_TEST(ScriptMqtt, NothingIsPublishedWhenThereIsNoBroker) {
    Rig rig;
    rig.gateway.setConnected(false);
    STIPPLE_CHECK(!rig.gateway.connected());
    STIPPLE_CHECK(!rig.gateway.publish("a", "b", "c", false));
    STIPPLE_CHECK(rig.client.published.empty());
}

STIPPLE_TEST(ScriptMqtt, AnOversizedPayloadIsRefusedRatherThanSent) {
    Rig rig;
    const std::string huge(IScriptMqtt::kMaxPayloadBytes + 1, 'x');
    STIPPLE_CHECK(!rig.gateway.publish("a", "b", huge, false));
    STIPPLE_CHECK(rig.client.published.empty());
}

STIPPLE_TEST(ScriptMqtt, AWatchSubscribesOnceAndIsIdempotent) {
    // Scripts call watch() from draw(), because there is nowhere else to call
    // it from. So it has to be safe thirty times a second for ever.
    Rig rig;
    for (int i = 0; i < 50; ++i) {
        STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    }
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.subscribed.size()), 1);
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 1);
}

STIPPLE_TEST(ScriptMqtt, AScriptIsToldWhenItsWatchListIsFull) {
    // Silently dropping the seventh would show up as one reading on a
    // dashboard that never updates, which is the hardest kind of bug to find.
    Rig rig;
    for (int i = 0; i < IScriptMqtt::kMaxWatchesPerScript; ++i) {
        const std::string filter = "home/sensor" + std::to_string(i);
        STIPPLE_REQUIRE(rig.gateway.watch("greedy", filter));
    }
    STIPPLE_CHECK(!rig.gateway.watch("greedy", "home/one-too-many"));

    // Another script still gets its own allowance.
    STIPPLE_CHECK(rig.gateway.watch("polite", "home/kettle"));
}

STIPPLE_TEST(ScriptMqtt, NothingArrivedIsNotAnEmptyPayload) {
    // ADR 0013, one layer out. A sensor that published "" and a sensor that
    // has said nothing since boot are different states.
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    STIPPLE_CHECK(rig.gateway.latest("solar", "home/solar/power") == nullptr);
    STIPPLE_CHECK(rig.gateway.ageMillis("solar", "home/solar/power") < 0);

    rig.arrive("home/solar/power", "", 1000);
    const std::string* value = rig.gateway.latest("solar", "home/solar/power");
    STIPPLE_REQUIRE(value != nullptr);
    STIPPLE_CHECK(value->empty());
    STIPPLE_CHECK(rig.gateway.ageMillis("solar", "home/solar/power") == 0);
}

STIPPLE_TEST(ScriptMqtt, AgeIsHowAStaleRetainedMessageIsCaught) {
    // A retained message from a sensor whose battery died three weeks ago
    // arrives the instant the device connects and looks exactly like a live
    // reading. Age is the only thing that tells them apart.
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    rig.arrive("home/solar/power", "1420", 5000);

    rig.gateway.setNowMillis(65000);
    STIPPLE_CHECK(rig.gateway.ageMillis("solar", "home/solar/power") == 60000);
}

STIPPLE_TEST(ScriptMqtt, AScriptOnlySeesWhatItAskedFor) {
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    rig.arrive("home/kettle/state", "on", 100);

    STIPPLE_CHECK(rig.gateway.latest("solar", "home/solar/power") == nullptr);
    // And cannot read another script's watch even by naming its filter.
    STIPPLE_CHECK(rig.gateway.latest("nosy", "home/solar/power") == nullptr);
}

STIPPLE_TEST(ScriptMqtt, AWildcardWatchKeepsTheMostRecentMatch) {
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("rooms", "home/+/temperature"));
    rig.arrive("home/kitchen/temperature", "21", 100);
    rig.arrive("home/study/temperature", "19", 200);

    const std::string* value = rig.gateway.latest("rooms", "home/+/temperature");
    STIPPLE_REQUIRE(value != nullptr);
    STIPPLE_CHECK(*value == "19");
}

STIPPLE_TEST(ScriptMqtt, AnOversizedArrivalIsTruncatedRatherThanDropped) {
    // The field a script wanted is usually near the front of a JSON document,
    // and a silent nothing would look identical to the sensor going quiet.
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("big", "home/dump"));
    const std::string huge(IScriptMqtt::kMaxPayloadBytes * 4, 'y');
    rig.arrive("home/dump", huge.c_str(), 10);

    const std::string* value = rig.gateway.latest("big", "home/dump");
    STIPPLE_REQUIRE(value != nullptr);
    STIPPLE_CHECK_EQ(static_cast<int>(value->size()),
                     static_cast<int>(IScriptMqtt::kMaxPayloadBytes));
}

STIPPLE_TEST(ScriptMqtt, GoingOfflineDoesNotBlankWhatWasAlreadyKnown) {
    // The last reading is still the last reading, and the script can see from
    // its age that it is getting old. Blanking it would turn one broker outage
    // into every script on the device losing its data at once.
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    rig.arrive("home/solar/power", "1420", 1000);

    rig.gateway.setConnected(false);
    const std::string* value = rig.gateway.latest("solar", "home/solar/power");
    STIPPLE_REQUIRE(value != nullptr);
    STIPPLE_CHECK(*value == "1420");
}

STIPPLE_TEST(ScriptMqtt, AReconnectResendsEveryWatch) {
    // A broker that restarted has forgotten the session, and without this the
    // scripts go quiet in a way indistinguishable from the sensors behind them
    // having stopped publishing.
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("a", "home/one"));
    STIPPLE_REQUIRE(rig.gateway.watch("b", "home/two"));
    rig.client.subscribed.clear();

    rig.gateway.setConnected(false);
    rig.gateway.setConnected(true);
    rig.gateway.resubscribe();

    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.subscribed.size()), 2);
}

STIPPLE_TEST(ScriptMqtt, AWatchMadeWhileOfflineIsSentWhenTheBrokerReturns) {
    Rig rig;
    rig.gateway.setConnected(false);
    // watch() still succeeds offline - the script has said what it wants, and
    // refusing would mean every script had to retry until the broker appeared.
    STIPPLE_REQUIRE(rig.gateway.watch("solar", "home/solar/power"));
    STIPPLE_CHECK(rig.client.subscribed.empty());

    rig.gateway.setConnected(true);
    rig.gateway.resubscribe();
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.subscribed.size()), 1);
}

STIPPLE_TEST(ScriptMqtt, ForgettingAScriptReleasesItsWatches) {
    Rig rig;
    STIPPLE_REQUIRE(rig.gateway.watch("old", "home/one"));
    STIPPLE_REQUIRE(rig.gateway.watch("keep", "home/two"));

    rig.gateway.forget("old");
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 1);
    STIPPLE_CHECK(rig.gateway.latest("old", "home/one") == nullptr);

    // And the survivor is untouched.
    rig.arrive("home/two", "still here", 10);
    const std::string* value = rig.gateway.latest("keep", "home/two");
    STIPPLE_REQUIRE(value != nullptr);
    STIPPLE_CHECK(*value == "still here");
}

// --- the builtins ------------------------------------------------------------

namespace {

/// Compile `source` into `store` and draw one frame.
bool drawOnce(ScriptStore& store, const char* id, const char* source,
              Framebuffer& framebuffer, std::uint64_t elapsedMillis = 0) {
    if (store.put(id, id, source) != ScriptPutResult::Added) {
        return false;
    }
    Canvas canvas(framebuffer);
    return store.draw(id, canvas, elapsedMillis);
}

}  // namespace

STIPPLE_TEST(ScriptMqttBuiltins, ADeviceWithNoBrokerSaysSoRatherThanPretending) {
    ScriptStore store;  // no gateway at all
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "quiet", R"BE(
class App
  def draw()
    if !mqtt_known()
      text(0, 0, 'off', rgb(255, 0, 0))
    end
    if mqtt_publish('state', 'on')
      pixel(51, 15, rgb(0, 255, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));

    STIPPLE_CHECK(countLit(framebuffer) > 0);
    STIPPLE_CHECK(framebuffer.at(51, 15) == colors::kBlack);
}

STIPPLE_TEST(ScriptMqttBuiltins, AScriptPublishesUnderItsOwnId) {
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "doorbell", R"BE(
class App
  def draw()
    mqtt_publish('rung', 'yes')
  end
end
return App()
)BE",
                             framebuffer));

    STIPPLE_REQUIRE(rig.client.published.size() == 1);
    STIPPLE_CHECK(rig.client.published[0].topic ==
                  "stipple/kitchen/script/doorbell/rung");
}

STIPPLE_TEST(ScriptMqttBuiltins, AScriptCannotFloodTheBroker) {
    // A script looping over a publish is a device flooding somebody's home
    // automation from inside their own network.
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "spam", R"BE(
class App
  def draw()
    var i = 0
    while i < 500
      mqtt_publish('noise', str(i))
      i += 1
    end
  end
end
return App()
)BE",
                             framebuffer));

    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.published.size()),
                     IScriptMqtt::kMaxPublishesPerCall);

    // The budget refills, so one greedy frame does not silence the script for
    // ever.
    rig.client.published.clear();
    Canvas canvas(framebuffer);
    STIPPLE_REQUIRE(store.draw("spam", canvas, 33));
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.published.size()),
                     IScriptMqtt::kMaxPublishesPerCall);
}

STIPPLE_TEST(ScriptMqttBuiltins, AScriptReadsWhatItWatched) {
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    const char* source = R"BE(
class App
  def draw()
    mqtt_watch('home/solar/power')
    var v = mqtt_get('home/solar/power')
    if v == nil
      text(0, 0, '--', rgb(90, 90, 90))
    else
      text(0, 0, v, rgb(0, 255, 0))
    end
  end
end
return App()
)BE";

    STIPPLE_REQUIRE(drawOnce(store, "solar", source, framebuffer));
    const int beforeAnything = countLit(framebuffer);
    STIPPLE_CHECK(beforeAnything > 0);  // drew the "--"

    rig.arrive("home/solar/power", "1420", 100);

    Framebuffer after;
    Canvas canvas(after);
    STIPPLE_REQUIRE(store.draw("solar", canvas, 33));
    STIPPLE_CHECK(!(after == framebuffer));
}

STIPPLE_TEST(ScriptMqttBuiltins, AgeIsVisibleToAScript) {
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "stale", R"BE(
class App
  def draw()
    mqtt_watch('home/solar/power')
    var age = mqtt_age_ms('home/solar/power')
    if age < 0
      pixel(0, 0, rgb(80, 80, 80))
    elif age > 60000
      pixel(1, 0, rgb(255, 140, 0))
    else
      pixel(2, 0, rgb(0, 255, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK(framebuffer.at(0, 0) != colors::kBlack);  // nothing yet

    rig.arrive("home/solar/power", "1420", 1000);
    rig.gateway.setNowMillis(2000);
    Framebuffer fresh;
    Canvas freshCanvas(fresh);
    STIPPLE_REQUIRE(store.draw("stale", freshCanvas, 33));
    STIPPLE_CHECK(fresh.at(2, 0) != colors::kBlack);  // live

    rig.gateway.setNowMillis(1000 * 60 * 60);
    Framebuffer old;
    Canvas oldCanvas(old);
    STIPPLE_REQUIRE(store.draw("stale", oldCanvas, 66));
    STIPPLE_CHECK(old.at(1, 0) != colors::kBlack);  // an hour stale
}

STIPPLE_TEST(ScriptMqttBuiltins, DeletingAScriptReleasesItsSubscriptions) {
    // Otherwise a script that has been gone for a month still has the device
    // subscribed on its behalf, and the only way anybody finds out is by
    // reading the broker's subscription list.
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "temp", R"BE(
class App
  def draw()
    mqtt_watch('home/kitchen/temperature')
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 1);

    STIPPLE_REQUIRE(store.remove("temp"));
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 0);
}

STIPPLE_TEST(ScriptMqttBuiltins, SavingAnEditStartsFromNoWatches) {
    // The new source may want different topics, and carrying the old ones over
    // leaves the device subscribed on behalf of code that no longer exists.
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "temp", R"BE(
class App
  def draw()
    mqtt_watch('home/old')
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 1);

    STIPPLE_REQUIRE(store.put("temp", "temp", R"BE(
class App
  def draw()
    mqtt_watch('home/new')
  end
end
return App()
)BE") == ScriptPutResult::Replaced);
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 0);

    Canvas canvas(framebuffer);
    STIPPLE_REQUIRE(store.draw("temp", canvas, 33));
    STIPPLE_CHECK_EQ(rig.gateway.watchCount(), 1);
    STIPPLE_CHECK(rig.gateway.latest("temp", "home/old") == nullptr);
}

STIPPLE_TEST(ScriptMqttBuiltins, OneScriptCannotReadAnothersTopics) {
    Rig rig;
    ScriptStore store;
    store.setMqtt(&rig.gateway);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "owner", R"BE(
class App
  def draw()
    mqtt_watch('home/secret')
  end
end
return App()
)BE",
                             framebuffer));
    rig.arrive("home/secret", "hunter2", 100);

    STIPPLE_REQUIRE(drawOnce(store, "nosy", R"BE(
class App
  def draw()
    var v = mqtt_get('home/secret')
    if v != nil
      pixel(0, 0, rgb(255, 0, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));

    STIPPLE_CHECK(framebuffer.at(0, 0) == colors::kBlack);
}
