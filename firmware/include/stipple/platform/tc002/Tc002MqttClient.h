// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "stipple/platform/MqttClient.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// MQTT 3.1.1 over a non-blocking POSIX socket, polled from the render loop.
///
/// Single-threaded for the same reason as Tc002HttpServer: everything above
/// this line — the bridge, the carousel, the config store — is unsynchronised,
/// and a broker thread delivering a message into it would be a race nobody
/// would find twice. IMqttClient already has poll(), so unlike the HTTP
/// transport this needs no addition to say so.
///
/// Reconnection is deliberately *not* here. The interface says an unreachable
/// broker is a state change rather than a failure, and mqtt::MqttService owns
/// the backoff policy. This reports what happened and nothing more.
///
/// QoS 0 and 1 only, matching MqttMessage. QoS 2's four-way handshake would
/// cost per-message state on a device with 16 MB free to solve a problem a
/// pixel clock does not have.
///
/// **No TLS.** MqttConnectOptions::tls is a request, and the interface is
/// explicit that an adapter which cannot honour it must fail the connection
/// rather than quietly send credentials in the clear. That is what this does.
class Tc002MqttClient final : public IMqttClient {
public:
    /// Outbound messages held while the socket is busy. Bounded per §38:
    /// publish() returns false when full, because growing a queue to avoid
    /// saying no is exactly how that rule gets broken.
    static constexpr int kSendQueueDepth = 16;

    /// Caps one inbound packet. Larger than any scene or icon payload this
    /// device accepts over HTTP, and far below anything that threatens memory.
    static constexpr std::size_t kMaxPacketBytes = 32 * 1024;

    Tc002MqttClient() = default;
    ~Tc002MqttClient() override;

    Tc002MqttClient(const Tc002MqttClient&) = delete;
    Tc002MqttClient& operator=(const Tc002MqttClient&) = delete;

    bool connect(const MqttConnectOptions& options, IMqttListener& listener) override;
    void disconnect() override;
    MqttState state() const override { return state_; }
    bool publish(const MqttMessage& message) override;
    bool subscribe(std::string_view topicFilter, int qos) override;
    void poll(std::uint64_t nowMillis) override;

    /// Not yet. See the class comment: the TLS exists on this device, but
    /// this transport is polled from the render loop and cannot use it as it
    /// stands.
    bool supportsTls() const override { return false; }

    /// Messages refused because the queue was full. Diagnostics only.
    std::uint32_t droppedCount() const noexcept { return dropped_; }

private:
    void setState(MqttState state);
    void closeSocket() noexcept;
    void fail(const char* why);

    bool beginConnect();
    bool finishConnect();
    void sendConnectPacket();

    void pumpWrites();
    void pumpReads(std::uint64_t nowMillis);
    void handlePacket(const std::uint8_t* packet, std::size_t length);

    /// Appends to the outbound buffer. Returns false if that would exceed the
    /// bound, which is the only way this refuses work.
    bool enqueue(const std::string& bytes);

    /// Next packet identifier, skipping zero, which the spec forbids.
    std::uint16_t takePacketId();

    int fd_ = -1;
    MqttState state_ = MqttState::Disabled;
    IMqttListener* listener_ = nullptr;
    MqttConnectOptions options_;

    /// True between the socket being created and CONNACK arriving.
    bool awaitingConnack_ = false;

    std::string outbound_;
    std::size_t sent_ = 0;
    std::string inbound_;

    std::uint16_t nextPacketId_ = 1;
    std::uint64_t lastActivityMillis_ = 0;
    std::uint64_t connectStartedMillis_ = 0;
    std::uint32_t dropped_ = 0;
};

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
