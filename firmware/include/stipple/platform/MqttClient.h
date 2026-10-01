// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace stipple {
namespace platform {

/// One message, either direction.
struct MqttMessage {
    std::string topic;
    std::string payload;
    bool retained = false;
    /// 0 or 1. QoS 2 is deliberately not offered: it costs a four-way handshake
    /// and per-message state on a device with an unmeasured RAM budget, to solve
    /// a problem this product does not have — a duplicated "show a notification"
    /// is a far smaller harm than a stalled broker session.
    int qos = 0;
};

/// What a connection is currently doing. Reported rather than inferred, because
/// "not connected" and "never configured" need different answers in the UI.
enum class MqttState : std::uint8_t {
    Disabled,      ///< no broker configured, or MQTT switched off
    Disconnected,  ///< configured, not currently connected
    Connecting,
    Connected,
};

const char* mqttStateName(MqttState state) noexcept;

struct MqttConnectOptions {
    std::string host;
    int port = 1883;
    std::string clientId;
    std::string username;
    std::string password;

    /// Last will, published by the broker if this device drops off without
    /// saying goodbye. Availability that depends on the device being well enough
    /// to announce its own death is not availability.
    std::string willTopic;
    std::string willPayload;
    bool willRetained = true;

    int keepAliveSeconds = 30;

    /// TLS is a §46 unknown on this hardware, so this is a request rather than a
    /// guarantee: an adapter that cannot do TLS must fail the connection rather
    /// than quietly fall back to plaintext with credentials on it.
    bool tls = false;
};

/// Receives inbound messages. Core provides the implementation; the transport
/// does not know what any topic means.
class IMqttListener {
public:
    virtual ~IMqttListener() = default;
    virtual void onMessage(const MqttMessage& message) = 0;
    virtual void onStateChanged(MqttState state) = 0;
};

/// The MQTT transport: sockets, protocol framing, keepalive.
///
/// Below the platform boundary for the same reason as IHttpServer — what the
/// TC002 can actually open a socket with is unknown until Phase 7, and the
/// browser has no sockets at all. Everything about what a topic *means* lives
/// above this line in `mqtt::Bridge`, so neither answer changes the semantics.
///
/// **No adapter implements this yet.** `IPlatformServices::mqtt()` returns
/// nullptr everywhere, which is the honest report (ADR 0013) rather than a stub
/// that accepts publish() and drops it.
class IMqttClient {
public:
    virtual ~IMqttClient() = default;

    /// Begin connecting. Returns false only for arguments the adapter can see
    /// are unusable; an unreachable broker is a later state change, not a
    /// failure here, because the reconnect policy is core's job.
    ///
    /// `listener` must outlive the client.
    virtual bool connect(const MqttConnectOptions& options, IMqttListener& listener) = 0;

    virtual void disconnect() = 0;
    virtual MqttState state() const = 0;

    /// Returns false when not connected or the send queue is full. Callers must
    /// treat a false as "this message is gone" — §38 forbids an unbounded queue,
    /// and silently growing one to avoid saying no is how that rule gets broken.
    virtual bool publish(const MqttMessage& message) = 0;

    virtual bool subscribe(std::string_view topicFilter, int qos) = 0;

    /// Pump the transport. Called from the application loop, so nothing arrives
    /// on a thread the rest of the firmware does not know about.
    virtual void poll(std::uint64_t nowMillis) = 0;

    /// Whether this adapter can honour `MqttConnectOptions::tls`.
    ///
    /// Asked so a UI can say so before somebody turns it on, rather than
    /// after. An adapter without TLS must refuse the connection rather than
    /// downgrade - that part was already right - but refusing is
    /// indistinguishable from an unreachable broker to the person watching,
    /// and "it just stopped working when I ticked the box" is not a
    /// diagnosis. ADR 0013: a capability that is absent has to be visible as
    /// absent.
    ///
    /// Pure rather than defaulted to false, so a new adapter has to answer
    /// for itself instead of inheriting a claim that quietly goes stale.
    virtual bool supportsTls() const = 0;
};

}  // namespace platform
}  // namespace stipple
