// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "stipple/api/Http.h"
#include "stipple/json/Json.h"

namespace stipple {

namespace app {
class AppRegistry;
class Carousel;
}  // namespace app

namespace apps {
namespace glucose {
class NightscoutSource;
}
}  // namespace apps

namespace notify {
class NotificationQueue;
}

namespace render {
class FrameScheduler;
}

namespace asset {
class IconStore;
}

namespace script {
class IScriptRunner;
struct Script;
}  // namespace script

namespace api {
class JsonWriter;
}

namespace log {
class RingLog;
}

namespace config {
struct Config;
class ConfigStore;
}  // namespace config

namespace platform {
class IPlatformServices;
class IInputSink;
}

class Framebuffer;

namespace api {

/// Everything the API is allowed to touch.
///
/// Passed in rather than reached for through a global, so a test can wire up
/// exactly the subset it needs and a null pointer means "this build does not
/// offer that", not a crash.
struct ApiContext {
    /// Whether this device has never been configured.
    ///
    /// A pointer to the host's own flag rather than a copy: it stops being
    /// true the moment anything is saved, and a copy taken at construction
    /// would still be claiming first run after the first save.
    const bool* firstRun = nullptr;

    app::AppRegistry* apps = nullptr;
    app::Carousel* carousel = nullptr;
    notify::NotificationQueue* notifications = nullptr;
    asset::IconStore* icons = nullptr;

    /// Optional. Null means this build has no scripting, which the script
    /// endpoints report honestly rather than pretending to an empty library -
    /// "you have no scripts" and "this device cannot run scripts" are very
    /// different answers to somebody whose script is not showing up.
    script::IScriptRunner* scripts = nullptr;
    config::Config* config = nullptr;
    config::ConfigStore* configStore = nullptr;
    platform::IPlatformServices* platform = nullptr;
    log::RingLog* logger = nullptr;

    /// The most recently rendered frame, for the live view.
    ///
    /// Read from here rather than from IFrameBufferDisplay, which is
    /// deliberately write-only — a display is somewhere pixels go, and adding a
    /// read-back would oblige every adapter to keep a copy it otherwise has no
    /// use for. The host already owns exactly one framebuffer; this points at
    /// it.
    const Framebuffer* frame = nullptr;

    /// Where an injected press goes. Null disables the input endpoint rather
    /// than accepting presses that vanish.
    platform::IInputSink* input = nullptr;

    /// Render pacing, for diagnostics. Null simply omits the section: a build
    /// that does not schedule frames has nothing truthful to say about them.
    const render::FrameScheduler* scheduler = nullptr;

    /// The glucose app's data source, for diagnostics: counts, the last HTTP
    /// status, the last failure. Never its URL and never its credential. Null
    /// omits the section.
    const apps::glucose::NightscoutSource* glucose = nullptr;
};

struct ApiOptions {
    /// When set, every request must present it. Empty disables authentication,
    /// which is the default for a LAN-only device (§23) but should be a
    /// deliberate choice, not an accident.
    std::string authToken;

    /// Requests larger than this are rejected before parsing. Bodies arrive
    /// from the network and must not be able to exhaust RAM (§38).
    std::size_t maxBodyBytes = 16u * 1024u;

    /// The ceiling for a firmware image, which is the one thing that
    /// legitimately dwarfs every other request.
    ///
    /// Separate from `maxBodyBytes` rather than raising it: everything else
    /// on this API is small, and a single limit generous enough for a
    /// firmware image would let any request allocate megabytes. This device
    /// has 36 MB of RAM and roughly 17 MB of it free.
    ///
    /// Four MiB, because the res partition is eight and an image for it is
    /// compressed - the factory one is 2.8 MB. Anything past this is not a
    /// firmware image for this device.
    std::size_t maxImageBytes = 4u * 1024u * 1024u;

    /// The ceiling for an icon, which is the other thing that legitimately
    /// dwarfs an ordinary request.
    ///
    /// Pixels arrive as a JSON array of packed 0xRRGGBB integers, so the
    /// body is roughly nine bytes per pixel. A 32x32 sixteen-frame animation
    /// - the largest geometry the store accepts - is 16,384 pixels and
    /// therefore about 150 KB of JSON. 256 KB covers it with room for the
    /// id and the frame structure.
    ///
    /// Separate from `maxBodyBytes` for the same reason a firmware image is:
    /// everything else on this API is small, and one limit generous enough
    /// for an animation would let any request allocate a quarter of a
    /// megabyte.
    std::size_t maxIconBytes = 256u * 1024u;

    /// Token budget for parsing a request body.
    int maxJsonTokens = 512;

    /// And for an icon, where every pixel is a token.
    ///
    /// This is the limit people actually hit, and it is invisible when they
    /// do: 512 tokens is two 16x16 frames, so somebody who upscaled an 8x8
    /// icon set from a TC001 gets "too many JSON tokens" on an icon that is
    /// well inside every size limit the page shows them.
    ///
    /// 32x32 x 16 frames is 16,384 pixels; 20,000 covers that and the
    /// structure around it. A token is twelve bytes, so the worst case is
    /// 240 KB held for the length of one parse.
    int maxIconJsonTokens = 20000;
};

/// The native `/api/v1/*` surface (blueprint §19.1).
///
/// Pure: a request in, a response out. No sockets and no clock of its own —
/// `nowMillis` is supplied, so every endpoint is reproducible in a test.
class ApiServer {
public:
    ApiServer(const ApiContext& context, ApiOptions options = ApiOptions{})
        : context_(context), options_(std::move(options)) {}

    Response handle(const Request& request, std::uint64_t nowMillis);

    const ApiOptions& options() const noexcept { return options_; }

    /// Hand the API a script runner after construction.
    ///
    /// The context is built when the host is constructed and the runner is
    /// installed after, because whoever owns the interpreter is outside the
    /// core and cannot exist before it. Without this the API would hold the
    /// null it was born with and report no scripting on a device that has it.
    void setScriptRunner(script::IScriptRunner* runner) noexcept {
        context_.scripts = runner;
    }

public:
    /// Tokens for validating a scene as it arrives.
    ///
    /// The same budget the host renders with, so the API cannot accept a
    /// scene the renderer would later reject for being too complex. Six
    /// kilobytes, held rather than taken from the stack: this runs on the
    /// thread that also draws the panel.
    static constexpr int kSceneTokens = 512;

private:
    /// Checks the token when one is configured.
    ///
    /// Applies to every endpoint including health. An unauthenticated liveness
    /// probe is convenient, but it also leaks uptime, version and app names to
    /// anything on the LAN, and §23 asks for the security-conscious default.
    /// Monitoring must present the token.
    bool authorised(const Request& request) const;

    Response handleDevice(const Request& request);
    Response handleHealth(const Request& request, std::uint64_t nowMillis);
    Response handleVersion(const Request& request);
    Response handleDiagnostics(const Request& request, std::uint64_t nowMillis);
    Response handleLogs(const Request& request);

    Response handleAppCollection(const Request& request, std::uint64_t nowMillis);
    Response handleAppItem(const Request& request, const std::string& id,
                           std::uint64_t nowMillis);
    Response handleAppActivate(const Request& request, const std::string& id,
                               std::uint64_t nowMillis);

    Response handleNotificationCollection(const Request& request, std::uint64_t nowMillis);
    Response handleNotificationItem(const Request& request, const std::string& id,
                                    std::uint64_t nowMillis);

    Response handleAssetCollection(const Request& request);
    Response handleAssetItem(const Request& request, const std::string& id);
    /// Parse a scene and describe what is wrong with it.
    ///
    /// Fills `warnings` with what failed validation, and returns false when
    /// *none* of the elements survived - a scene that can only ever draw a
    /// black panel.
    ///
    /// This exists because the scene was previously taken on trust: anything
    /// that was a JSON object was stored and answered 201. A Domoticz
    /// integration sent perfectly reasonable elements in a shape the renderer
    /// did not accept, got a cheerful success for every one of them, and had
    /// no way to discover why its panel was black.
    bool describeScene(std::string_view json, std::vector<std::string>& warnings);

    json::Token sceneTokens_[kSceneTokens];

    Response handleScriptCollection(const Request& request);
    Response handleScriptItem(const Request& request, const std::string& id);

    Response handleSettings(const Request& request);
    Response handleReboot(const Request& request);
    Response handleReset(const Request& request, std::uint64_t nowMillis);
    Response handleNetwork(const Request& request);
    Response handleNetworkScan(const Request& request);
    Response handleNetworkJoin(const Request& request);
    Response handleFirmware(const Request& request);

    Response handleDisplayFrame(const Request& request);
    Response handleInput(const Request& request, std::uint64_t nowMillis);

    ApiContext context_;
    ApiOptions options_;
};

}  // namespace api
}  // namespace stipple
