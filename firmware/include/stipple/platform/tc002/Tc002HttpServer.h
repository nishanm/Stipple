// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "stipple/platform/HttpServer.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// HTTP over POSIX sockets, polled from the application loop.
///
/// **Single-threaded on purpose.** ApplicationHost is built around a
/// caller-driven `tick()` with, in its own words, no threading primitives
/// anywhere near the renderer. A server thread calling `ApiServer::handle()`
/// would put untrusted network input on a second thread touching the carousel,
/// the config store and the icon store, none of which are synchronised. So
/// every socket here is non-blocking and `poll()` does one pass per frame.
///
/// That is also why `poll()` is on this class rather than on IHttpServer: a
/// transport that owned a thread would need no such call, and the interface
/// should not force one on an implementation that does.
///
/// The cost is bounded latency rather than throughput — a request waits at most
/// one frame interval (15 ms) to be noticed, which is irrelevant for a config
/// page and a REST API, and cheap next to the correctness it buys.
///
/// Everything here is bounded, per blueprint §38: a fixed connection table, a
/// maximum request size, and a deadline that closes a connection which stops
/// making progress. A device with one panel does not need to serve a crowd, and
/// an unbounded server on 64 MB of RAM is a denial of service waiting to be
/// discovered.
class Tc002HttpServer final : public IHttpServer {
public:
    /// Connections served at once. A browser opens up to six to one host, so
    /// fewer than that and a single page load can find the table full.
    static constexpr int kMaxConnections = 8;

    /// Connections the kernel holds, completed but not yet accepted, while the
    /// table is full. They wait a poll or two and are then served, instead of
    /// being accepted and closed unanswered - which is what a burst of six
    /// requests met when the table was four and full meant "close".
    static constexpr int kListenBacklog = 16;

    /// Caps a single request, headers and body together. Larger than any
    /// legitimate scene or icon payload and far smaller than anything that
    /// would threaten memory.
    static constexpr std::size_t kMaxRequestBytes = 64 * 1024;

    /// The one request allowed to be enormous: a firmware image.
    ///
    /// **Earned, not granted.** Raising kMaxRequestBytes to this would let
    /// eight connections hold thirty-odd megabytes between them, more than
    /// the free RAM on this device. Instead a connection only gets this
    /// ceiling once its first line shows it is the upload, and only one
    /// connection at a time may have it: a second upload while one is
    /// arriving is held to the small ceiling and refused.
    ///
    /// Four MiB plus room for headers: the res partition is eight, and an
    /// image for it is compressed - the factory one is 2.8 MB.
    static constexpr std::size_t kMaxUploadBytes = 4u * 1024u * 1024u + 64u * 1024u;

    /// Matched against the start of a request to decide whether the larger
    /// ceiling applies. Spelled out rather than routed, because this has to
    /// be decided from the first packet, long before there is a parsed
    /// request to route.
    static constexpr const char* kUploadRequestLine =
        "POST /api/v1/system/firmware";

    /// A connection that has not made progress in this long is dropped. Without
    /// it a half-open socket holds a slot until reboot.
    static constexpr std::uint64_t kIdleTimeoutMillis = 15000;

    Tc002HttpServer() = default;
    ~Tc002HttpServer() override;

    Tc002HttpServer(const Tc002HttpServer&) = delete;
    Tc002HttpServer& operator=(const Tc002HttpServer&) = delete;

    bool start(int port, IHttpRequestHandler& handler) override;
    void stop() override;
    bool running() const override { return listenFd_ >= 0; }

private:
    struct Connection {
        int fd = -1;
        std::string inbound;
        std::string outbound;
        std::size_t sent = 0;
        std::uint64_t lastProgressMillis = 0;
        /// Set once a response has been queued; the socket closes when it has
        /// been written, rather than being kept alive for another request.
        bool closing = false;
    };

    /// How much this connection is allowed to accumulate. The larger figure
    /// applies only to an upload, and only while no other connection is
    /// already using it.
    std::size_t ceilingFor(const Connection& connection) const;
    static bool isUpload(const std::string& inbound) noexcept;
    bool hasFreeSlot() const noexcept;

public:
    int port() const override { return port_; }

    /// One non-blocking pass: accept what is waiting, read what has arrived,
    /// answer what is complete, write what is pending. Never blocks.
    void poll(std::uint64_t nowMillis);

    /// Requests answered since start. Diagnostics only.
    std::uint32_t servedCount() const noexcept { return served_; }

    /// Connections dropped for exceeding a bound rather than completing.
    std::uint32_t rejectedCount() const noexcept { return rejected_; }

private:

    void acceptPending(std::uint64_t nowMillis);
    void service(Connection& connection, std::uint64_t nowMillis);
    void closeConnection(Connection& connection) noexcept;

    /// Parses what has arrived so far. Returns true once a whole request is
    /// present, and fills `request`.
    bool tryParse(const std::string& raw, api::Request& request, bool& malformed) const;

    void queueResponse(Connection& connection, const api::Response& response) const;

    int listenFd_ = -1;
    int port_ = 0;
    IHttpRequestHandler* handler_ = nullptr;
    Connection connections_[kMaxConnections];
    std::uint32_t served_ = 0;
    std::uint32_t rejected_ = 0;
};

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
