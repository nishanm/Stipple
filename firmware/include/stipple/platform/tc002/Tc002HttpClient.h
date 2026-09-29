// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "stipple/platform/HttpClient.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// One HTTP GET at a time, on a worker thread.
///
/// **A thread rather than a non-blocking state machine**, which is the
/// opposite of the choice Tc002MqttClient made, and deliberately so. MQTT is a
/// connection held open for days with a keepalive to answer, so its state
/// machine earns its complexity. This makes one request every few minutes and
/// then forgets about it, and the non-blocking version of that is a hundred
/// lines of partial-write handling to save a thread that exists for two
/// seconds. Tc002Sntp already resolves names the same way, for the same
/// reason.
///
/// The thread is detached and talks back only through a shared block of
/// atomics, so a request that never returns cannot hold the panel up or
/// outlive the storage it writes into. That is the property that makes this
/// safe, and it is why the result block is a `shared_ptr` rather than a member.
///
/// **No TLS.** An `https://` URL is refused, plainly and in words, rather than
/// downgraded to plaintext. Silently fetching over http what a script author
/// asked to fetch over https would put their API key on the wire of a network
/// they believed was protected — the same call Tc002MqttClient makes about a
/// broker password. The device can do TLS (it carries OpenSSL), so this is a
/// gap to fill and not a wall; until it is filled, saying so is the only
/// honest behaviour. See `docs/research/tc002-platform-findings.md`.
class Tc002HttpClient final : public IHttpClient {
public:
    /// How long the worker gives the whole exchange.
    ///
    /// Connect, send and read share it. Ten seconds is long for a local
    /// server and short enough that a wedged fetch frees its slot before
    /// anybody notices a feed has stopped.
    static constexpr int kTimeoutSeconds = 10;

    /// Bytes read off the socket before giving up on the rest, for a request
    /// with the default body cap.
    ///
    /// Larger than the body cap because headers count towards it, and a
    /// server that sends three kilobytes of cookies before a 200-byte body is
    /// ordinary rather than hostile. A request asking for a larger body raises
    /// this by the same amount, so the cap it asked for is the cap it gets.
    static constexpr std::size_t kMaxTransferBytes = 32u * 1024u;

    ~Tc002HttpClient() override;

    using IHttpClient::begin;
    bool begin(const HttpRequest& request) override;
    void poll(std::uint64_t nowMillis) override;
    Stage stage() const noexcept override { return stage_; }
    int status() const noexcept override { return status_; }
    std::string_view body() const noexcept override { return body_; }
    std::string_view failure() const noexcept override { return failure_; }
    void reset() override;

private:
    /// What the worker writes and the loop reads.
    ///
    /// Held by `shared_ptr` on both sides so a detached worker finishing after
    /// this object is gone writes into memory that is still its own.
    struct Exchange {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        std::atomic<int> status{0};

        /// Written before `done` is set and read only after it is seen, which
        /// is what makes a plain string safe here without a lock.
        std::string body;
        std::string failure;
    };

    std::shared_ptr<Exchange> exchange_;
    Stage stage_ = Stage::Idle;
    int status_ = 0;
    std::string body_;
    std::string failure_;
};

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
