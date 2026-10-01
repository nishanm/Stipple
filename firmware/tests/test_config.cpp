// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/config/Config.h"

#include <string>
#include <vector>

#include "stipple/json/Json.h"

#include "stipple/app/AppRegistry.h"
#include "stipple/apps/ClockApp.h"
#include "stipple/core/Checksum.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::config::AppPreference;
using stipple::config::Config;
using stipple::config::ConfigStore;
using stipple::config::kCurrentSchemaVersion;
using stipple::config::LoadReport;
using stipple::config::LoadStatus;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

int status(LoadStatus value) {
    return static_cast<int>(value);
}

/// Wrap a body in a correctly-checksummed envelope, so tests can hand-write
/// stored documents — including ones from older schema versions.
std::string envelope(const std::string& body) {
    char hex[9];
    stipple::crc32ToHex(stipple::crc32(body), hex);
    return std::string("{\"checksum\":\"") + hex + "\",\"body\":" + body + "}";
}

}  // namespace

// --- round trip --------------------------------------------------------------

STIPPLE_TEST(Config, SavesAndLoads) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.deviceName = "kitchen-clock";
    written.display.brightness = 200;
    written.display.power = false;
    written.apps.defaultDurationSeconds = 12;
    written.apps.transitions = false;
    written.clock.twentyFourHour = false;
    written.clock.utcOffsetSeconds = 3600;

    STIPPLE_CHECK(store.save(written));

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(read.deviceName, std::string("kitchen-clock"));
    STIPPLE_CHECK_EQ(static_cast<int>(read.display.brightness), 200);
    STIPPLE_CHECK_FALSE(read.display.power);
    STIPPLE_CHECK_EQ(read.apps.defaultDurationSeconds, 12);
    STIPPLE_CHECK_FALSE(read.apps.transitions);
    STIPPLE_CHECK_FALSE(read.clock.twentyFourHour);
    STIPPLE_CHECK_EQ(read.clock.utcOffsetSeconds, 3600);
}

STIPPLE_TEST(Config, ClockStyleSurvivesARoundTrip) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.clock.theme = "calendar";
    written.clock.leadingZero = false;
    written.clock.showAmPm = true;
    written.clock.color = 0x123456u;
    written.clock.accentColor = 0xABCDEFu;
    written.clock.dateColor = 0x0000FFu;
    written.clock.dateOrder = "yearMonthDay";
    written.clock.dateSeparator = "dash";
    written.clock.dateYear = "twoDigit";
    written.clock.blinkPeriodMillis = 250;
    STIPPLE_CHECK(store.save(written));

    Config read;
    STIPPLE_CHECK_EQ(status(store.load(read).status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(read.clock.theme, std::string("calendar"));
    STIPPLE_CHECK_FALSE(read.clock.leadingZero);
    STIPPLE_CHECK(read.clock.showAmPm);
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.color), 0x123456);
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.accentColor), 0xABCDEF);
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.dateColor), 0x0000FF);
    STIPPLE_CHECK_EQ(read.clock.dateOrder, std::string("yearMonthDay"));
    STIPPLE_CHECK_EQ(read.clock.dateSeparator, std::string("dash"));
    STIPPLE_CHECK_EQ(read.clock.dateYear, std::string("twoDigit"));
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.blinkPeriodMillis), 250);
}

STIPPLE_TEST(Config, EveryDefaultClockNameRoundTripsThroughTheAppLayer) {
    // The settings API accepts only names that survive name -> enum -> name. A
    // default that did not round-trip would be rejected by the same endpoint
    // that reports it, which is a maddening bug to find from the outside.
    const Config defaults;
    STIPPLE_CHECK_EQ(
        std::string(stipple::apps::clockThemeName(
            stipple::apps::clockThemeFromName(defaults.clock.theme))),
        defaults.clock.theme);
    STIPPLE_CHECK_EQ(
        std::string(stipple::apps::dateOrderName(
            stipple::apps::dateOrderFromName(defaults.clock.dateOrder))),
        defaults.clock.dateOrder);
    STIPPLE_CHECK_EQ(
        std::string(stipple::apps::dateSeparatorName(
            stipple::apps::dateSeparatorFromName(defaults.clock.dateSeparator))),
        defaults.clock.dateSeparator);
    STIPPLE_CHECK_EQ(
        std::string(stipple::apps::dateYearName(
            stipple::apps::dateYearFromName(defaults.clock.dateYear))),
        defaults.clock.dateYear);
}

STIPPLE_TEST(Config, AMalformedColourKeepsTheDefaultRatherThanFailingTheLoad) {
    // One bad field must not cost the user every other setting they have.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(
        ConfigStore::kPrimaryKey,
        envelope(R"({"schemaVersion":2,"deviceName":"kept",
                     "clock":{"color":"not-a-colour","accentColor":"#00FF00"}})"));

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(read.deviceName, std::string("kept"));
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.color), 0xFFFFFF);   // default
    STIPPLE_CHECK_EQ(static_cast<int>(read.clock.accentColor), 0x00FF00);  // applied
}

STIPPLE_TEST(Config, BlinkPeriodIsClampedButZeroIsPreserved) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    const auto loadedBlink = [&](const char* raw) {
        platform.storage().write(
            ConfigStore::kPrimaryKey,
            envelope(std::string(R"({"schemaVersion":2,"clock":{"blinkPeriodMillis":)") + raw +
                     "}}"));
        Config read;
        store.load(read);
        return static_cast<int>(read.clock.blinkPeriodMillis);
    };

    // 0 is not "unset"; it means hold the colon lit, so it must survive.
    STIPPLE_CHECK_EQ(loadedBlink("0"), 0);
    STIPPLE_CHECK_EQ(loadedBlink("-5"), 0);
    STIPPLE_CHECK_EQ(loadedBlink("10"), 100);
    STIPPLE_CHECK_EQ(loadedBlink("999999"), 60000);
    STIPPLE_CHECK_EQ(loadedBlink("1500"), 1500);
}

STIPPLE_TEST(Config, FirstBootUsesDefaults) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config config;
    const LoadReport report = store.load(config);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::DefaultsMissing));
    STIPPLE_CHECK_EQ(config.schemaVersion, kCurrentSchemaVersion);
    STIPPLE_CHECK_EQ(config.deviceName, std::string("stipple"));
}

STIPPLE_TEST(Config, DeviceNameWithAwkwardCharactersSurvives) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.deviceName = "he said \"hi\"\\\n\tdone";
    STIPPLE_CHECK(store.save(written));

    Config read;
    store.load(read);
    STIPPLE_CHECK_EQ(read.deviceName, written.deviceName);
}

// --- integrity ---------------------------------------------------------------

STIPPLE_TEST(Config, DetectsCorruptedPayload) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.display.brightness = 77;
    store.save(written);

    // Flip a byte inside the body. The checksum exists for exactly this.
    std::string stored;
    platform.storage().read(ConfigStore::kPrimaryKey, stored);
    const std::size_t digit = stored.find("77");
    STIPPLE_CHECK(digit != std::string::npos);
    stored[digit] = '8';
    platform.storage().write(ConfigStore::kPrimaryKey, stored);

    // Nothing was ever saved before this, so there is no backup to fall back to
    // and the load lands on defaults. The point is that the tampering is caught
    // rather than loaded as if it were genuine.
    Config read;
    const LoadReport report = store.load(read);
    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::DefaultsCorrupt));
    STIPPLE_CHECK_EQ(static_cast<int>(read.display.brightness), 128);
}

STIPPLE_TEST(Config, RecoversFromBackupWhenPrimaryIsCorrupt) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config first;
    first.deviceName = "original";
    store.save(first);

    Config second;
    second.deviceName = "updated";
    store.save(second);  // pushes "original" into the backup key

    platform.storage().write(ConfigStore::kPrimaryKey, "{ this is not json");

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::RecoveredFromBackup));
    STIPPLE_CHECK(report.usedBackup);
    STIPPLE_CHECK_EQ(read.deviceName, std::string("original"));
}

STIPPLE_TEST(Config, BothCopiesCorruptFallsBackToDefaults) {
    // The rule that matters: a malformed configuration must never brick the
    // clock or cause a boot loop.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(ConfigStore::kPrimaryKey, "garbage");
    platform.storage().write(ConfigStore::kBackupKey, "also garbage");

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::DefaultsCorrupt));
    STIPPLE_CHECK_EQ(read.deviceName, std::string("stipple"));
}

STIPPLE_TEST(Config, TruncatedPayloadIsRejected) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    store.save(written);

    std::string stored;
    platform.storage().read(ConfigStore::kPrimaryKey, stored);

    // Every truncation must be refused, never half-applied.
    for (std::size_t length = 1; length < stored.size(); length += 7) {
        platform.storage().write(ConfigStore::kPrimaryKey, stored.substr(0, length));
        Config read;
        const LoadReport report = store.load(read);
        STIPPLE_CHECK(report.status != LoadStatus::Loaded);
    }
}

STIPPLE_TEST(Config, ArbitraryGarbageNeverCrashes) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    const char* samples[] = {
        "", "null", "[]", "{}", "{\"checksum\":1}", R"({"checksum":"zz","body":{}})",
        R"({"body":{"schemaVersion":2}})", R"({"checksum":"00000000","body":[]})",
        "\x01\x02\x03", R"({"checksum":"00000000","body":{"schemaVersion":"two"}})",
    };

    for (const char* sample : samples) {
        platform.storage().write(ConfigStore::kPrimaryKey, sample);
        Config read;
        store.load(read);
        STIPPLE_CHECK(read.schemaVersion == kCurrentSchemaVersion);
    }
}

// --- schema versions ---------------------------------------------------------

STIPPLE_TEST(Config, MigratesBrightnessFromPercentToBytes) {
    // v1 stored brightness as 0-100. Loading one must convert, not clamp.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(
        ConfigStore::kPrimaryKey,
        envelope(R"({"schemaVersion":1,"deviceName":"old","display":{"brightness":50}})"));

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::Migrated));
    STIPPLE_CHECK_EQ(report.fromSchemaVersion, 1);
    STIPPLE_CHECK_EQ(static_cast<int>(read.display.brightness), 128);
    STIPPLE_CHECK_EQ(read.deviceName, std::string("old"));
}

STIPPLE_TEST(Config, MigrationCoversTheFullRange) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    const auto migratedBrightness = [&](int percent) {
        platform.storage().write(ConfigStore::kPrimaryKey,
                                 envelope(R"({"schemaVersion":1,"display":{"brightness":)" +
                                          std::to_string(percent) + "}}"));
        Config read;
        store.load(read);
        return static_cast<int>(read.display.brightness);
    };

    STIPPLE_CHECK_EQ(migratedBrightness(0), 0);
    STIPPLE_CHECK_EQ(migratedBrightness(100), 255);
}

STIPPLE_TEST(Config, MigratedConfigIsRewrittenAtTheCurrentSchema) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(ConfigStore::kPrimaryKey,
                             envelope(R"({"schemaVersion":1,"display":{"brightness":100}})"));

    Config read;
    store.load(read);
    STIPPLE_CHECK_EQ(read.schemaVersion, kCurrentSchemaVersion);

    // Saving it back stores the new schema, so the migration happens once.
    STIPPLE_CHECK(store.save(read));
    Config again;
    const LoadReport report = store.load(again);
    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(static_cast<int>(again.display.brightness), 255);
}

STIPPLE_TEST(Config, RefusesNewerSchema) {
    // Downgraded firmware must not guess at fields whose meaning has changed.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(ConfigStore::kPrimaryKey,
                             envelope(R"({"schemaVersion":99,"deviceName":"future"})"));

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::DefaultsFutureSchema));
    STIPPLE_CHECK_EQ(report.fromSchemaVersion, 99);
    STIPPLE_CHECK_EQ(read.deviceName, std::string("stipple"));
}

STIPPLE_TEST(Config, IgnoresUnknownFields) {
    // A config written by a newer minor build should still load.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(
        ConfigStore::kPrimaryKey,
        envelope(R"({"schemaVersion":2,"deviceName":"x","somethingNew":{"a":[1,2]},
                     "display":{"brightness":90,"futureField":true}})"));

    Config read;
    const LoadReport report = store.load(read);

    STIPPLE_CHECK_EQ(status(report.status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(static_cast<int>(read.display.brightness), 90);
}

// --- validation --------------------------------------------------------------

STIPPLE_TEST(Config, ClampsOutOfRangeValues) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(
        ConfigStore::kPrimaryKey,
        envelope(R"({"schemaVersion":2,"display":{"brightness":99999},
                     "apps":{"defaultDurationSeconds":-5},
                     "clock":{"utcOffsetSeconds":999999}})"));

    Config read;
    store.load(read);

    STIPPLE_CHECK_EQ(static_cast<int>(read.display.brightness), 255);
    STIPPLE_CHECK_EQ(read.apps.defaultDurationSeconds, 1);
    STIPPLE_CHECK_EQ(read.clock.utcOffsetSeconds, 14 * 3600);
}

STIPPLE_TEST(Config, SaveRejectsOversizedPayload) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.deviceName = std::string(platform.storage().maxValueBytes() + 1, 'n');
    STIPPLE_CHECK_FALSE(store.save(written));
}

STIPPLE_TEST(Config, TokenBudgetHasHeadroom) {
    // Overflowing kMaxTokens makes a perfectly good configuration read as
    // corrupt, and the device silently falls back to defaults -- losing every
    // setting the user had. Each field added since this was written eats into
    // the margin, so measure it with the real parser rather than by eye.
    Config config;
    config.deviceName = std::string(64, 'n');
    config.mqtt.host = "broker.example.invalid";
    config.mqtt.clientId = "stipple-kitchen";
    config.mqtt.username = "user";
    config.mqtt.password = "secret";

    // A full registry of remembered apps, which is what a maximal document
    // actually looks like now. Each one is about seven tokens, so this is more
    // of the budget than everything above it put together - and leaving it out
    // would have made this test measure a document no real device writes.
    for (int i = 0; i < stipple::config::kMaxRememberedApps; ++i) {
        AppPreference preference;
        preference.id = std::string(stipple::app::AppRegistry::kMaxIdBytes, 'a');
        preference.enabled = (i % 2) == 0;
        preference.durationSeconds = 3600;
        config.apps.order.push_back(std::move(preference));
    }

    // And every glucose alarm with its full eight alert windows and the
    // longest melody text allowed - the other large part of a real document.
    for (stipple::config::AlarmRule* rule :
         {&config.glucose.alarms.urgentLow, &config.glucose.alarms.low,
          &config.glucose.alarms.high, &config.glucose.alarms.noData}) {
        rule->windowCount = stipple::config::AlarmRule::kMaxWindows;
        for (int i = 0; i < rule->windowCount; ++i) {
            rule->windows[i].dayMask = 0x7F;
            rule->windows[i].fromMinutes = 22 * 60;
            rule->windows[i].toMinutes = 7 * 60;
        }
        rule->melody = std::string(256, 'c');
    }

    const std::string payload = ConfigStore::serialize(config);

    // Find what a maximal document actually costs, by parsing it at rising
    // budgets until it fits.
    int needed = 0;
    for (int budget = 1; budget <= ConfigStore::kMaxTokens; ++budget) {
        std::vector<stipple::json::Token> tokens(static_cast<std::size_t>(budget));
        stipple::json::Document document(tokens.data(), budget);
        if (document.parse(payload) == stipple::json::Error::None) {
            needed = budget;
            break;
        }
    }

    STIPPLE_CHECK(needed > 0);  // it fits at all
    // Half the budget spare. Tighter than that and the next few settings would
    // silently push a real device over.
    STIPPLE_CHECK(needed <= ConfigStore::kMaxTokens / 2);

    // And the end-to-end proof, which is what actually matters.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());
    STIPPLE_CHECK(store.save(config));

    Config read;
    STIPPLE_CHECK_EQ(status(store.load(read).status), status(LoadStatus::Loaded));
    STIPPLE_CHECK_EQ(read.mqtt.host, config.mqtt.host);
    STIPPLE_CHECK_EQ(read.deviceName, config.deviceName);
}

STIPPLE_TEST(Config, MqttSettingsRoundTrip) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.mqtt.enabled = true;
    written.mqtt.host = "broker.local";
    written.mqtt.port = 8883;
    written.mqtt.clientId = "kitchen";
    written.mqtt.baseTopic = "home";
    written.mqtt.username = "user";
    written.mqtt.password = "secret";
    written.mqtt.tls = true;
    written.mqtt.keepAliveSeconds = 45;
    written.mqtt.discovery = true;
    STIPPLE_CHECK(store.save(written));

    Config read;
    STIPPLE_CHECK_EQ(status(store.load(read).status), status(LoadStatus::Loaded));
    STIPPLE_CHECK(read.mqtt.enabled);
    STIPPLE_CHECK_EQ(read.mqtt.host, std::string("broker.local"));
    STIPPLE_CHECK_EQ(read.mqtt.port, 8883);
    STIPPLE_CHECK_EQ(read.mqtt.baseTopic, std::string("home"));
    // The credential has to survive storage, or the device cannot reconnect.
    STIPPLE_CHECK_EQ(read.mqtt.password, std::string("secret"));
    STIPPLE_CHECK(read.mqtt.tls);
    STIPPLE_CHECK_EQ(read.mqtt.keepAliveSeconds, 45);
}

STIPPLE_TEST(Config, MqttValuesAreClamped) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    platform.storage().write(
        ConfigStore::kPrimaryKey,
        envelope(R"({"schemaVersion":2,"mqtt":{"port":999999,"keepAliveSeconds":0,
                     "baseTopic":""}})"));

    Config read;
    store.load(read);

    STIPPLE_CHECK_EQ(read.mqtt.port, 65535);
    STIPPLE_CHECK_EQ(read.mqtt.keepAliveSeconds, 5);
    // An empty base would publish to "/{deviceId}/status".
    STIPPLE_CHECK_EQ(read.mqtt.baseTopic, std::string("stipple"));
}

STIPPLE_TEST(Config, EveryStatusHasADescription) {
    for (int i = 0; i <= static_cast<int>(LoadStatus::DefaultsFutureSchema); ++i) {
        const char* text = stipple::config::describe(static_cast<LoadStatus>(i));
        STIPPLE_CHECK(text != nullptr && text[0] != '\0');
    }
}

STIPPLE_TEST(Config, RememberedAppsMatchWhatTheRegistryCanHold) {
    // kMaxRememberedApps is duplicated rather than included, to keep
    // configuration from depending on the app layer for one number. That is
    // only safe if something notices when the two drift.
    STIPPLE_CHECK_EQ(stipple::config::kMaxRememberedApps,
                    stipple::app::AppRegistry::kMaxApps);
}

STIPPLE_TEST(Config, AppOrderSurvivesASaveAndLoad) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    AppPreference first;
    first.id = "battery";
    first.enabled = false;
    first.durationSeconds = 12;
    written.apps.order.push_back(first);

    AppPreference second;
    second.id = "clock";
    written.apps.order.push_back(second);

    STIPPLE_REQUIRE(store.save(written));

    Config read;
    store.load(read);

    STIPPLE_REQUIRE(read.apps.order.size() == 2);
    STIPPLE_CHECK_EQ(read.apps.order[0].id, std::string("battery"));
    STIPPLE_CHECK_FALSE(read.apps.order[0].enabled);
    STIPPLE_CHECK_EQ(read.apps.order[0].durationSeconds, 12);
    STIPPLE_CHECK_EQ(read.apps.order[1].id, std::string("clock"));
    STIPPLE_CHECK(read.apps.order[1].enabled);
}

STIPPLE_TEST(Config, AnOrderEntryWithNoIdIsSkippedOnLoad) {
    // An entry naming nothing orders nothing, and keeping it would leave a
    // permanent no-op sitting in the user's arrangement. Driven through save()
    // rather than by hand-writing the document, because a stored document
    // carries a checksum and one written by hand is simply rejected - which
    // would have made this test pass for the wrong reason.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    AppPreference nameless;          // id left empty
    nameless.enabled = true;
    written.apps.order.push_back(nameless);

    AppPreference real;
    real.id = "clock";
    written.apps.order.push_back(real);

    STIPPLE_REQUIRE(store.save(written));

    Config read;
    store.load(read);

    STIPPLE_REQUIRE(read.apps.order.size() == 1);
    STIPPLE_CHECK_EQ(read.apps.order[0].id, std::string("clock"));
}

STIPPLE_TEST(Config, NightSettingsRoundTrip) {
    // Nesting matters: the serialiser wrote this at the top level while the
    // parser read it from inside "display", so it saved and never came back -
    // and nothing else in the suite would have noticed.
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.display.night.enabled = true;
    written.display.night.startMinutes = 23 * 60 + 15;
    written.display.night.endMinutes = 6 * 60 + 30;
    written.display.night.brightness = 9;
    STIPPLE_REQUIRE(store.save(written));

    Config read;
    store.load(read);

    STIPPLE_CHECK(read.display.night.enabled);
    STIPPLE_CHECK_EQ(read.display.night.startMinutes, 23 * 60 + 15);
    STIPPLE_CHECK_EQ(read.display.night.endMinutes, 6 * 60 + 30);
    STIPPLE_CHECK_EQ(static_cast<int>(read.display.night.brightness), 9);
}

STIPPLE_TEST(Config, ATimeOutsideADayIsClamped) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.display.night.startMinutes = 99999;
    written.display.night.endMinutes = -5;
    STIPPLE_REQUIRE(store.save(written));

    Config read;
    store.load(read);
    STIPPLE_CHECK(read.display.night.startMinutes >= 0 && read.display.night.startMinutes <= 1439);
    STIPPLE_CHECK(read.display.night.endMinutes >= 0 && read.display.night.endMinutes <= 1439);
}
