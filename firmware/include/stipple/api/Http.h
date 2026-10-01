// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stipple {
namespace api {

/// HTTP as a pure data transformation.
///
/// A request goes in, a response comes out. No sockets, no threads, no
/// platform. That keeps every endpoint testable without a network stack, lets
/// the device and the simulator share one implementation, and means the
/// transport can be swapped without touching a single handler — which matters
/// because the TC002's transport is still unknown (§46).

enum class Method : std::uint8_t {
    Unknown,
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Options,
    Head,
};

Method methodFromName(std::string_view name) noexcept;
const char* methodName(Method method) noexcept;

struct Request {
    Method method = Method::Unknown;

    /// Path only, without the query string. Expected to be already
    /// percent-decoded by the transport.
    std::string path;

    /// Raw query string, without the leading '?'.
    std::string query;

    std::string body;

    /// Bearer token or X-API-Key value, extracted by the transport. Kept
    /// separate from the body so no handler has to know how it arrived.
    std::string authToken;

    /// The Authorization header, verbatim.
    ///
    /// Carried whole rather than pre-split, because Basic is decided in the
    /// core (ADR 0018) - the transport's job is to hand over what arrived,
    /// not to decide what it means. That also means the simulator and the
    /// browser emulator enforce exactly what the device does.
    std::string authorization;

    /// If-None-Match, for conditional requests. Named rather than reached for
    /// through a header map: the set of headers this device understands is small
    /// and fixed, and spelling it out keeps it auditable — the same reasoning
    /// that keeps matchRoute() a list of paths instead of a pattern engine.
    std::string ifNoneMatch;

    /// Value of a query parameter, or `fallback` when absent.
    std::string queryValue(std::string_view name, std::string_view fallback = {}) const;
    bool hasQuery(std::string_view name) const;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;

    /// Emitted as ETag and Cache-Control when non-empty. Only the static file
    /// handler sets these; API responses describe live state and are deliberately
    /// not cacheable.
    std::string etag;
    std::string cacheControl;

    /// Emitted as WWW-Authenticate when non-empty.
    ///
    /// This is the whole reason a browser shows a password box rather than a
    /// bare 401 page: without the header the request just fails, and the
    /// person has no way to supply what is missing.
    std::string wwwAuthenticate;
};

// --- response helpers --------------------------------------------------------

Response ok(std::string body);
Response created(std::string body);
Response noContent();

/// Errors share one shape: {"error":{"code":"...","message":"..."}}.
///
/// A machine-readable code with a human-readable message, so an integration can
/// branch on the code while a person debugging sees something useful. Blueprint
/// §40 forbids showing raw errors on the panel; this is the API's side of that.
Response error(int status, std::string_view code, std::string_view message);

Response badRequest(std::string_view message);
Response unauthorized(std::string_view message = "missing or invalid API token");
Response notFound(std::string_view message = "not found");
Response methodNotAllowed(std::string_view message = "method not allowed for this resource");
Response conflict(std::string_view message);
Response payloadTooLarge(std::string_view message = "request body too large");
Response unprocessable(std::string_view message);
Response serverError(std::string_view message);

// --- routing -----------------------------------------------------------------

/// Every addressable thing in the native API (blueprint §19.1).
enum class Resource : std::uint8_t {
    Unknown,
    Device,
    Health,
    Version,
    Diagnostics,
    Logs,
    AppCollection,
    AppItem,
    AppActivate,
    NotificationCollection,
    NotificationItem,
    AssetCollection,
    AssetItem,
    /// Berry scripts. Separate from apps because the two are edited by
    /// different people for different reasons: an app is a carousel entry
    /// anyone can reorder, a script is source code somebody is writing.
    ScriptCollection,
    ScriptItem,
    Settings,
    SystemReboot,
    /// Put configuration back to defaults. Separate from a DELETE on settings
    /// because "reset" and "delete" are different promises: this leaves a
    /// working configuration behind rather than an absent one.
    SystemReset,
    /// What the device can see, and what it is on. Read-only: joining is a
    /// provisioning concern with its own gates (ADR 0018).
    Network,
    /// Ask the radio to look. A POST because it does something.
    NetworkScan,
    NetworkJoin,
    /// Install a new STIPPLE. Replaced SystemRestoreImage, which staged an
    /// image the vendor's recovery daemon would install unattended.
    SystemFirmware,
    /// The frame currently on the panel, for the web UI's live view.
    DisplayFrame,
    /// A button press injected from somewhere that is not the hardware.
    Input,
    /// Play a glucose alarm melody now, so a settings page can let somebody
    /// hear what they chose. POST /api/v1/glucose/alarm/test.
    GlucoseAlarmTest,
};

struct RouteMatch {
    Resource resource = Resource::Unknown;
    /// The `{id}` segment, for item routes.
    std::string id;
};

/// Match a path to a resource.
///
/// Deliberately not a generic pattern engine: the route set is small, fixed and
/// public API surface. Spelling it out keeps every reachable path visible in one
/// place, which matters when the thing being routed is untrusted network input.
RouteMatch matchRoute(std::string_view path);

/// Longest API version prefix this build serves.
inline constexpr std::string_view kApiV1Prefix = "/api/v1";

}  // namespace api
}  // namespace stipple
