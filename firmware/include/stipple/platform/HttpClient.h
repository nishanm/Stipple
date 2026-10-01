// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stipple/net/HttpFetch.h"

namespace stipple {
namespace platform {

/// What a fetch asks for, beyond the URL.
///
/// One optional header, as a name and a value rather than raw header text, so
/// an adapter can check the two things that matter - a token for the name, no
/// control characters in the value - instead of scanning for line breaks. And
/// a body cap per request: the default is the parser's kilobyte, which suits a
/// script drawing a headline; a source that needs a whole document says so and
/// pays for it in memory, once, knowingly.
struct HttpRequest {
    std::string_view url;
    std::string_view headerName;
    std::string_view headerValue;
    std::size_t maxBodyBytes = net::http::ResponseParser::kMaxBodyBytes;

    /// "GET" (the default) or "POST". Glucose services log in with a POST;
    /// nothing else here has a reason to send anything.
    std::string_view method = "GET";

    /// Sent with a POST. Bounded by kMaxRequestBodyBytes - a login form.
    std::string_view body;

    /// Further headers, at most net::http::kMaxRequestHeaders, each checked
    /// as `headerName` is. A `User-Agent` here replaces the default.
    const net::http::Header* headers = nullptr;
    std::size_t headerCount = 0;
};

/// A request body is a login form or a small JSON document.
inline constexpr std::size_t kMaxRequestBodyBytes = 2048;

/// Every check a client makes before it sends anything: method, header
/// names and values, counts and sizes. Both adapters call this, so the
/// simulator refuses exactly what the device refuses.
inline bool requestIsSafe(const HttpRequest& request) noexcept {
    if (!net::http::methodIsSafe(request.method)) {
        return false;
    }
    if (!net::http::headerIsSafe(request.headerName, request.headerValue)) {
        return false;
    }
    if (request.headerCount > net::http::kMaxRequestHeaders ||
        (request.headerCount > 0 && request.headers == nullptr)) {
        return false;
    }
    for (std::size_t i = 0; i < request.headerCount; ++i) {
        if (!net::http::headerIsSafe(request.headers[i].name, request.headers[i].value)) {
            return false;
        }
    }
    if (request.body.size() > kMaxRequestBodyBytes ||
        (request.method != "POST" && !request.body.empty())) {
        return false;
    }
    return true;
}

/// One outbound HTTP request at a time.
///
/// **One, not a pool, and that is the design rather than a shortcut.** This
/// device has 14 MB free and sixteen scripts. A pool would mean sixteen
/// sockets, sixteen response buffers and sixteen timeouts to get right, in
/// exchange for making a panel that redraws every 33 ms fetch two things at
/// once instead of one. The core schedules round-robin over this, which is
/// both simpler and bounded by construction.
///
/// Nothing here blocks. `begin` starts a fetch and returns; `poll` is called
/// from the application loop and is where all the work happens or is
/// collected. Blueprint §16 forbids anything on the render thread that can
/// wait on a network.
class IHttpClient {
public:
    enum class Stage : std::uint8_t {
        /// Nothing in flight.
        Idle,
        /// A fetch is running. Keep polling.
        Running,
        /// Finished. `status()` and `body()` hold the answer.
        Done,
        /// Finished badly. `failure()` says why, in words meant for a person.
        Failed,
    };

    virtual ~IHttpClient() = default;

    /// Start a GET.
    ///
    /// False when one is already running, when the URL or header is unusable,
    /// or when this platform cannot fetch that URL at all — an https URL on a
    /// build with no TLS, for instance. A false leaves `failure()` describing
    /// it, so the reason reaches the panel rather than the log alone.
    virtual bool begin(const HttpRequest& request) = 0;

    /// The common case: a URL and nothing else. Adapters override the
    /// request form and re-expose this one with `using IHttpClient::begin`.
    bool begin(std::string_view url) {
        HttpRequest request;
        request.url = url;
        return begin(request);
    }

    /// Drive it. Called once per frame; must return promptly every time.
    virtual void poll(std::uint64_t nowMillis) = 0;

    virtual Stage stage() const noexcept = 0;

    /// The HTTP status, once `stage()` is Done. Zero otherwise.
    virtual int status() const noexcept = 0;

    /// The body, once `stage()` is Done. Bounded by the adapter.
    virtual std::string_view body() const noexcept = 0;

    /// Why the last attempt failed, in words that can go on a 52-pixel panel
    /// or in a log a person will read. Empty when nothing has failed.
    virtual std::string_view failure() const noexcept = 0;

    /// The cookies the last response set, as `a=1; b=2`, ready to send back
    /// as a Cookie header. Empty when there were none.
    virtual std::string_view cookies() const noexcept { return {}; }

    /// Throw away the result and return to Idle, ready for the next fetch.
    virtual void reset() = 0;
};

}  // namespace platform
}  // namespace stipple
