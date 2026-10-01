// SPDX-License-Identifier: GPL-3.0-or-later
//
// `/api/v1/system/firmware` — and specifically the question "is what is on
// disk what is running".
//
// It reads like a detail and it is the difference between an update that
// worked and one that did nothing. An installed override looks identical
// whether it is waiting for a restart or already loaded: after the restart it
// is both, and `installedBytes` says the same number either way. The web page
// reported that as "Running an installed update (1306 KB). Version 0.2.3.",
// where the size described the file just uploaded and the version described
// the process still serving the page - two true halves that read as one
// sentence saying the update had taken effect.
//
// `restartPending()` is the narrow thing that can actually be answered: an
// install during this process's lifetime necessarily happened after this
// process loaded, so nothing it wrote is running yet.
#include "stipple/api/ApiServer.h"

#include <string>

#include "stipple/core/Version.h"
#include "stipple/json/Json.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::api::ApiContext;
using stipple::api::ApiServer;
using stipple::api::Method;
using stipple::api::Request;
using stipple::api::Response;
using stipple::platform::simulator::SimulatorPlatform;

namespace {

/// The smallest thing ElfCheck accepts: a 32-bit little-endian ARM shared
/// object header. The API refuses anything else before the adapter is
/// reached, so a test that installs has to get past it.
std::string armSharedObject(std::size_t padTo = 64) {
    std::string image;
    image += '\x7f';
    image += "ELF";
    image += '\x01';  // EI_CLASS: 32-bit
    image += '\x01';  // EI_DATA: little-endian
    image += '\x01';  // EI_VERSION
    image.resize(16, '\0');
    image += '\x03';
    image += '\x00';  // e_type = ET_DYN
    image += '\x28';
    image += '\x00';  // e_machine = EM_ARM (40)
    image.resize(padTo, '\0');
    return image;
}

/// An in-memory IUpgradeManager with the same state machine as the device
/// adapter, minus the filesystem. The simulator deliberately has no upgrade
/// manager at all - `/api/v1/system/firmware` answers 501 there - so a test
/// that exercises the route has to bring one.
class FakeUpgrade final : public stipple::platform::IUpgradeManager {
public:
    std::string applicationPath() const override { return "/data/stipple/libstipple.so.override"; }
    std::size_t installedBytes() const override { return bytes_; }
    bool hasPrevious() const override { return hasPrevious_; }

    bool install(std::string_view image, std::string& problem) override {
        problem.clear();
        if (image.empty()) {
            problem = "nothing to install";
            return false;
        }
        if (bytes_ > 0) {
            hasPrevious_ = true;
            previousBytes_ = bytes_;
        }
        bytes_ = image.size();
        restartPending_ = true;
        return true;
    }

    bool rollback(std::string& problem) override {
        problem.clear();
        if (!hasPrevious_) {
            problem = "there is no previous version to go back to";
            return false;
        }
        bytes_ = previousBytes_;
        hasPrevious_ = false;
        restartPending_ = true;
        return true;
    }

    bool restartPending() const override { return restartPending_; }

private:
    std::size_t bytes_ = 0;
    std::size_t previousBytes_ = 0;
    bool hasPrevious_ = false;
    bool restartPending_ = false;
};

class PlatformWithUpgrade final : public SimulatorPlatform {
public:
    stipple::platform::IUpgradeManager* upgrade() override { return &upgrade_; }
    FakeUpgrade upgrade_;
};

struct Fixture {
    PlatformWithUpgrade platform;
    ApiServer server;

    Fixture() : server(makeContext()) {}

    ApiContext makeContext() {
        ApiContext context;
        context.platform = &platform;
        return context;
    }

    Response call(const char* method, std::string body = std::string()) {
        Request request;
        request.method = stipple::api::methodFromName(method);
        request.path = "/api/v1/system/firmware";
        request.body = std::move(body);
        return server.handle(request, 0);
    }
};

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

STIPPLE_TEST(Firmware, ADeviceThatHasNeverBeenUpdatedSaysSoWithoutClaimingAnUpdate) {
    Fixture fixture;
    Parsed parsed(fixture.call("GET").body);
    STIPPLE_REQUIRE(parsed.ok);

    STIPPLE_CHECK_EQ(parsed.root()["installedBytes"].toInt(), std::int64_t(0));
    STIPPLE_CHECK(!parsed.root()["restartPending"].toBool(true));
    STIPPLE_CHECK(!parsed.root()["canRollBack"].toBool(true));
}

STIPPLE_TEST(Firmware, InstallingMakesARestartPending) {
    // The whole point. Before the install the process and the disk agree;
    // afterwards they do not, and only a restart reconciles them.
    Fixture fixture;
    STIPPLE_CHECK(!Parsed(fixture.call("GET").body).root()["restartPending"].toBool(true));

    const std::string image = armSharedObject(2048);
    const Response installed = fixture.call("POST", image);
    STIPPLE_REQUIRE(static_cast<int>(installed.status) == 200);

    Parsed body(installed.body);
    STIPPLE_REQUIRE(body.ok);
    STIPPLE_CHECK(body.root()["restartPending"].toBool(false));
    STIPPLE_CHECK(body.root()["rebootRequired"].toBool(false));
    // The version that is still running, so a page can name what is being
    // replaced instead of leaving somebody to read the size as the version.
    STIPPLE_CHECK(body.root()["runningVersion"].stringEquals(std::string(stipple::kVersion)));

    Parsed after(fixture.call("GET").body);
    STIPPLE_REQUIRE(after.ok);
    STIPPLE_CHECK(after.root()["restartPending"].toBool(false));
    STIPPLE_CHECK_EQ(after.root()["installedBytes"].toInt(), std::int64_t(image.size()));
    // Still the running process's version, which is exactly the trap: this
    // field never describes the file just installed.
    STIPPLE_CHECK(after.root()["version"].stringEquals(std::string(stipple::kVersion)));
}

STIPPLE_TEST(Firmware, ARollbackAlsoNeedsARestart) {
    // A rollback changes what loads next just as much as an install does, so
    // reporting it as settled would be the same lie in the other direction.
    Fixture fixture;
    STIPPLE_REQUIRE(fixture.call("POST", armSharedObject(1024)).status == 200);
    STIPPLE_REQUIRE(fixture.call("POST", armSharedObject(2048)).status == 200);

    Parsed rolled(fixture.call("DELETE").body);
    STIPPLE_REQUIRE(rolled.ok);
    STIPPLE_CHECK(rolled.root()["restartPending"].toBool(false));

    Parsed after(fixture.call("GET").body);
    STIPPLE_CHECK(after.root()["restartPending"].toBool(false));
    STIPPLE_CHECK_EQ(after.root()["installedBytes"].toInt(), std::int64_t(1024));
}

STIPPLE_TEST(Firmware, ARefusedInstallLeavesNothingPending) {
    // The asymmetry that matters: a device that reports a pending restart it
    // is not going to honour sends somebody to reboot for nothing, and the
    // reboot then "fixes" it by clearing a flag that was wrong.
    Fixture fixture;

    const Response hostBuild = fixture.call("POST", std::string("\x7f" "ELF\x02\x01\x01", 8) +
                                                    std::string(56, '\0'));
    STIPPLE_CHECK_EQ(static_cast<int>(hostBuild.status), 422);

    Parsed after(fixture.call("GET").body);
    STIPPLE_REQUIRE(after.ok);
    STIPPLE_CHECK(!after.root()["restartPending"].toBool(true));
    STIPPLE_CHECK_EQ(after.root()["installedBytes"].toInt(), std::int64_t(0));
}

STIPPLE_TEST(Firmware, APlatformWithNoUpgradeManagerSaysSoRatherThanReportingSettled) {
    // ADR 0013: absence is visible. A 501 is the honest answer; a 200 saying
    // "nothing pending" would read as "you are up to date".
    SimulatorPlatform bare;
    ApiContext context;
    context.platform = &bare;
    ApiServer server(context);

    Request request;
    request.method = Method::Get;
    request.path = "/api/v1/system/firmware";
    STIPPLE_CHECK_EQ(static_cast<int>(server.handle(request, 0).status), 501);
}
