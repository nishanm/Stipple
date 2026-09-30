// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <string>

#include "stipple/api/ApiServer.h"
#include "stipple/api/Http.h"
#include "stipple/apps/GlucoseSource.h"
#include "stipple/config/Config.h"
#include "stipple/core/Sha1.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::Sha1;
using stipple::api::ApiContext;
using stipple::api::ApiOptions;
using stipple::api::ApiServer;
using stipple::api::methodFromName;
using stipple::api::Request;
using stipple::api::Response;
using stipple::apps::glucose::NightscoutSource;
using stipple::config::Config;
using stipple::config::ConfigStore;
using stipple::config::LoadStatus;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

const char* const kSecret = "not-a-real-secret";
const char* const kUrl = "http://nightscout.example:1337";

std::string secretHash() {
    return Sha1::hex(kSecret, std::strlen(kSecret));
}

/// Just enough of the API to exercise settings and diagnostics.
struct Fixture {
    SimulatorPlatform platform;
    Config config;
    ConfigStore store{platform.storage()};
    NightscoutSource source;
    ApiServer server;

    Fixture() : server(context(), ApiOptions{}) {}

    ApiContext context() {
        ApiContext c;
        c.config = &config;
        c.configStore = &store;
        c.platform = &platform;
        c.glucose = &source;
        return c;
    }

    Response call(const char* method, const char* path, const std::string& body = {}) {
        Request request;
        request.method = methodFromName(method);
        request.path = path;
        request.body = body;
        return server.handle(request, 0);
    }
};

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

STIPPLE_TEST(GlucoseSettings, RoundTripThroughTheStore) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());

    Config written;
    written.glucose.url = kUrl;
    written.glucose.apiSecretSha1 = secretHash();
    written.glucose.pollSeconds = 90;
    written.glucose.face = "clock";
    STIPPLE_CHECK(store.save(written));

    Config read;
    STIPPLE_CHECK(store.load(read).status == LoadStatus::Loaded);
    STIPPLE_CHECK_EQ(read.glucose.url, std::string(kUrl));
    // The credential has to survive storage, or the device cannot fetch after a reboot.
    STIPPLE_CHECK_EQ(read.glucose.apiSecretSha1, secretHash());
    STIPPLE_CHECK_EQ(read.glucose.pollSeconds, 90);
    STIPPLE_CHECK_EQ(read.glucose.face, std::string("clock"));
}

STIPPLE_TEST(GlucoseSettings, PinnedRoundTripsAndIsChecked) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());
    Config written;
    written.glucose.pinned = false;
    STIPPLE_CHECK(store.save(written));
    Config read;
    STIPPLE_CHECK(store.load(read).status == LoadStatus::Loaded);
    STIPPLE_CHECK_FALSE(read.glucose.pinned);

    Fixture fixture;
    STIPPLE_CHECK(fixture.config.glucose.pinned);
    STIPPLE_CHECK(contains(fixture.call("GET", "/api/v1/settings").body, "\"pinned\":true"));
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"pinned":"yes"}})").status, 422);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"pinned":false}})").status, 200);
    STIPPLE_CHECK_FALSE(fixture.config.glucose.pinned);
}

STIPPLE_TEST(GlucoseSettings, TheDefaultsNameNothing) {
    // A public repository: no address, no credential, until somebody types one.
    const Config fresh;
    STIPPLE_CHECK(fresh.glucose.url.empty());
    STIPPLE_CHECK(fresh.glucose.apiSecretSha1.empty());
    STIPPLE_CHECK_EQ(fresh.glucose.pollSeconds, 60);
    STIPPLE_CHECK_EQ(fresh.glucose.face, std::string("hero"));
}

STIPPLE_TEST(GlucoseSettings, ThePollPeriodIsClampedOnLoad) {
    SimulatorPlatform platform;
    ConfigStore store(platform.storage());
    Config written;
    written.glucose.pollSeconds = 5;
    STIPPLE_CHECK(store.save(written));
    Config read;
    STIPPLE_CHECK(store.load(read).status == LoadStatus::Loaded);
    STIPPLE_CHECK_EQ(read.glucose.pollSeconds, 30);
}

STIPPLE_TEST(GlucoseSettings, TheApiNeverReturnsTheCredential) {
    // The hash is what Nightscout accepts, so the hash is the credential. The
    // MQTT password rule applies: a boolean, never the value, never a mask.
    Fixture fixture;
    fixture.config.glucose.url = kUrl;
    fixture.config.glucose.apiSecretSha1 = secretHash();

    const std::string body = fixture.call("GET", "/api/v1/settings").body;
    STIPPLE_CHECK(contains(body, "\"apiSecretSet\":true"));
    STIPPLE_CHECK(contains(body, kUrl));
    STIPPLE_CHECK_FALSE(contains(body, secretHash().c_str()));
    STIPPLE_CHECK_FALSE(contains(body, "apiSecretSha1"));
    STIPPLE_CHECK_FALSE(contains(body, kSecret));
}

STIPPLE_TEST(GlucoseSettings, TheSecretIsHashedOnReceiptAndNeverStoredAsTyped) {
    Fixture fixture;
    const std::string patch = std::string(R"({"glucose":{"apiSecret":")") + kSecret + "\"}}";
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", patch).status, 200);
    STIPPLE_CHECK_EQ(fixture.config.glucose.apiSecretSha1, secretHash());

    // What reached storage is the hash, and only the hash.
    std::string stored;
    STIPPLE_REQUIRE(fixture.platform.storage().read(ConfigStore::kPrimaryKey, stored));
    STIPPLE_CHECK(contains(stored, secretHash().c_str()));
    STIPPLE_CHECK_FALSE(contains(stored, kSecret));

    // An empty string is the only way to remove it.
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"apiSecret":""}})").status,
                     200);
    STIPPLE_CHECK(fixture.config.glucose.apiSecretSha1.empty());
    STIPPLE_CHECK(contains(fixture.call("GET", "/api/v1/settings").body, "\"apiSecretSet\":false"));
}

STIPPLE_TEST(GlucoseSettings, TheUrlIsValidatedAsItWillBeFetched) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"url":"nightscout.local"}})").status,
                     422);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"url":"ftp://x.local/"}})").status,
                     422);
    const std::string tooLong =
        std::string(R"({"glucose":{"url":"http://)") + std::string(250, 'n') + ".local\"}}";
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", tooLong).status, 422);

    // A trailing slash is tolerated and dropped, so the composed path has one slash.
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings",
                                  R"({"glucose":{"url":"http://nightscout.example:1337/"}})").status,
                     200);
    STIPPLE_CHECK_EQ(fixture.config.glucose.url, std::string(kUrl));

    // Empty disables the source rather than being refused.
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"url":""}})").status, 200);
    STIPPLE_CHECK(fixture.config.glucose.url.empty());
}

STIPPLE_TEST(GlucoseSettings, PeriodAndFaceAreChecked) {
    Fixture fixture;
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"pollSeconds":5}})").status, 422);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"pollSeconds":90}})").status, 200);
    STIPPLE_CHECK_EQ(fixture.config.glucose.pollSeconds, 90);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"face":"weather"}})").status, 422);
    STIPPLE_CHECK_EQ(fixture.call("PATCH", "/api/v1/settings", R"({"glucose":{"face":"big-graph"}})").status,
                     200);
    STIPPLE_CHECK_EQ(fixture.config.glucose.face, std::string("big-graph"));
}

STIPPLE_TEST(GlucoseSettings, DiagnosticsDescribeTheSourceWithoutItsUrlOrSecret) {
    Fixture fixture;
    fixture.config.glucose.url = kUrl;
    fixture.config.glucose.apiSecretSha1 = secretHash();
    fixture.source.configure(kUrl, secretHash(), 60);

    const Response response = fixture.call("GET", "/api/v1/diagnostics");
    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(contains(response.body, "\"glucose\""));
    STIPPLE_CHECK(contains(response.body, "\"configured\":true"));
    STIPPLE_CHECK(contains(response.body, "\"fetches\":0"));
    STIPPLE_CHECK_FALSE(contains(response.body, "nightscout.example"));
    STIPPLE_CHECK_FALSE(contains(response.body, secretHash().c_str()));
    STIPPLE_CHECK_FALSE(contains(response.body, kSecret));
}
