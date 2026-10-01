// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002MqttClient.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

// MQTT 3.1.1 control packet types, in the high nibble of byte 0.
constexpr std::uint8_t kConnect = 0x10;
constexpr std::uint8_t kConnack = 0x20;
constexpr std::uint8_t kPublish = 0x30;
constexpr std::uint8_t kPuback = 0x40;
constexpr std::uint8_t kSubscribe = 0x82;  // type 8, flags 0010 are mandatory
constexpr std::uint8_t kSuback = 0x90;
constexpr std::uint8_t kPingreq = 0xC0;
constexpr std::uint8_t kPingresp = 0xD0;
constexpr std::uint8_t kDisconnect = 0xE0;

/// A connect that has not completed in this long is treated as unreachable.
/// The interface leaves retrying to core, so this only has to stop waiting.
constexpr std::uint64_t kConnectTimeoutMillis = 10000;

void appendUint16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

/// MQTT strings are length-prefixed and never null-terminated.
void appendString(std::string& out, std::string_view text) {
    appendUint16(out, static_cast<std::uint16_t>(text.size()));
    out.append(text);
}

/// Remaining Length: seven bits per byte, high bit continues.
void appendRemainingLength(std::string& out, std::size_t length) {
    do {
        std::uint8_t digit = static_cast<std::uint8_t>(length % 128u);
        length /= 128u;
        if (length > 0) {
            digit = static_cast<std::uint8_t>(digit | 0x80u);
        }
        out.push_back(static_cast<char>(digit));
    } while (length > 0);
}

/// Decodes Remaining Length. Returns bytes consumed, 0 when incomplete, or -1
/// when malformed.
int readRemainingLength(const std::uint8_t* data, std::size_t available,
                        std::size_t& valueOut) {
    std::size_t value = 0;
    std::size_t multiplier = 1;

    for (int i = 0; i < 4; ++i) {
        if (static_cast<std::size_t>(i) >= available) {
            return 0;
        }
        const std::uint8_t digit = data[i];
        value += static_cast<std::size_t>(digit & 0x7Fu) * multiplier;
        if ((digit & 0x80u) == 0) {
            valueOut = value;
            return i + 1;
        }
        multiplier *= 128u;
    }
    return -1;  // five continuation bytes cannot be valid
}

bool setNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

Tc002MqttClient::~Tc002MqttClient() { closeSocket(); }

void Tc002MqttClient::setState(MqttState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (listener_ != nullptr) {
        listener_->onStateChanged(state_);
    }
}

void Tc002MqttClient::closeSocket() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    outbound_.clear();
    inbound_.clear();
    sent_ = 0;
    awaitingConnack_ = false;
}

void Tc002MqttClient::fail(const char*) {
    closeSocket();
    // Disconnected, not Disabled: the broker is still configured, and core's
    // policy decides whether and when to try again.
    setState(MqttState::Disconnected);
}

bool Tc002MqttClient::connect(const MqttConnectOptions& options, IMqttListener& listener) {
    listener_ = &listener;
    options_ = options;

    if (options_.host.empty()) {
        setState(MqttState::Disabled);
        return false;
    }

    // Refused outright rather than downgraded. Falling back to plaintext would
    // put the broker password on the wire of a network the user believed was
    // protected, which is worse than not connecting at all.
    //
    // Returning false rather than landing in Disconnected, because those mean
    // different things to the caller: Disconnected is "the broker is not
    // answering, try again later", and the reconnect policy will do exactly
    // that - forever, on a device that is never going to succeed. This is an
    // argument the adapter can see is unusable, which is what the interface
    // says false is for, and it is what stops the retry loop.
    if (options_.tls) {
        setState(MqttState::Disabled);
        return false;
    }

    closeSocket();
    setState(MqttState::Connecting);
    connectStartedMillis_ = 0;

    if (!beginConnect()) {
        fail("socket");
    }
    // True either way: an unreachable broker is a state change, not an
    // argument error, and the interface is explicit about that distinction.
    return true;
}

bool Tc002MqttClient::beginConnect() {
    // Numeric addresses only, and deliberately so: the device binary is
    // statically linked, and glibc cannot dlopen its NSS modules in a static
    // binary, so getaddrinfo would resolve nothing while appearing to work.
    // A hostname fails here, visibly, rather than mysteriously later.
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(options_.port));

    if (::inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
        return false;
    }

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    if (!setNonBlocking(fd_)) {
        return false;
    }

    // Small packets, sent promptly. Nagle would hold a PINGREQ back waiting for
    // company that never arrives.
    const int nodelay = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            return false;
        }
    }
    return true;
}

bool Tc002MqttClient::finishConnect() {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return false;
    }
    return error == 0;
}

void Tc002MqttClient::sendConnectPacket() {
    std::string variable;
    appendString(variable, "MQTT");
    variable.push_back(4);  // protocol level 4 is 3.1.1

    std::uint8_t flags = 0x02;  // clean session
    if (!options_.willTopic.empty()) {
        flags = static_cast<std::uint8_t>(flags | 0x04u);
        if (options_.willRetained) {
            flags = static_cast<std::uint8_t>(flags | 0x20u);
        }
    }
    if (!options_.username.empty()) {
        flags = static_cast<std::uint8_t>(flags | 0x80u);
    }
    if (!options_.password.empty()) {
        flags = static_cast<std::uint8_t>(flags | 0x40u);
    }
    variable.push_back(static_cast<char>(flags));
    appendUint16(variable, static_cast<std::uint16_t>(options_.keepAliveSeconds));

    appendString(variable, options_.clientId);
    if (!options_.willTopic.empty()) {
        appendString(variable, options_.willTopic);
        appendString(variable, options_.willPayload);
    }
    if (!options_.username.empty()) {
        appendString(variable, options_.username);
    }
    if (!options_.password.empty()) {
        appendString(variable, options_.password);
    }

    std::string packet;
    packet.push_back(static_cast<char>(kConnect));
    appendRemainingLength(packet, variable.size());
    packet += variable;

    enqueue(packet);
    awaitingConnack_ = true;
}

bool Tc002MqttClient::enqueue(const std::string& bytes) {
    // One bound covers everything outbound. Sixteen typical messages is the
    // intent; measuring it in bytes is what actually protects the heap.
    const std::size_t limit = static_cast<std::size_t>(kSendQueueDepth) * 1024u;
    if (outbound_.size() - sent_ + bytes.size() > limit) {
        ++dropped_;
        return false;
    }
    outbound_ += bytes;
    return true;
}

std::uint16_t Tc002MqttClient::takePacketId() {
    const std::uint16_t id = nextPacketId_++;
    if (nextPacketId_ == 0) {
        nextPacketId_ = 1;  // zero is not a valid packet identifier
    }
    return id;
}

bool Tc002MqttClient::publish(const MqttMessage& message) {
    if (state_ != MqttState::Connected) {
        return false;
    }

    const int qos = message.qos == 1 ? 1 : 0;

    std::string variable;
    appendString(variable, message.topic);
    if (qos == 1) {
        appendUint16(variable, takePacketId());
    }
    variable += message.payload;

    std::uint8_t header = static_cast<std::uint8_t>(kPublish | (qos << 1));
    if (message.retained) {
        header = static_cast<std::uint8_t>(header | 0x01u);
    }

    std::string packet;
    packet.push_back(static_cast<char>(header));
    appendRemainingLength(packet, variable.size());
    packet += variable;

    return enqueue(packet);
}

bool Tc002MqttClient::subscribe(std::string_view topicFilter, int qos) {
    if (state_ != MqttState::Connected) {
        return false;
    }

    std::string variable;
    appendUint16(variable, takePacketId());
    appendString(variable, topicFilter);
    variable.push_back(static_cast<char>(qos == 1 ? 1 : 0));

    std::string packet;
    packet.push_back(static_cast<char>(kSubscribe));
    appendRemainingLength(packet, variable.size());
    packet += variable;

    return enqueue(packet);
}

void Tc002MqttClient::disconnect() {
    if (fd_ >= 0 && state_ == MqttState::Connected) {
        // Written directly rather than queued: the socket is about to close and
        // a queued goodbye would never leave. Without it the broker publishes
        // the will, telling every subscriber the device died when it simply
        // went away.
        const std::uint8_t packet[2] = {kDisconnect, 0x00};
        ::send(fd_, packet, sizeof(packet), MSG_NOSIGNAL);
    }
    closeSocket();
    setState(MqttState::Disabled);
}

void Tc002MqttClient::pumpWrites() {
    while (sent_ < outbound_.size()) {
        const ssize_t wrote = ::send(fd_, outbound_.data() + sent_,
                                     outbound_.size() - sent_, MSG_NOSIGNAL);
        if (wrote > 0) {
            sent_ += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // finish on a later poll
        }
        fail("send");
        return;
    }

    // Fully flushed: reclaim, rather than letting the buffer grow forever.
    outbound_.clear();
    sent_ = 0;
}

void Tc002MqttClient::handlePacket(const std::uint8_t* packet, std::size_t length) {
    const std::uint8_t type = static_cast<std::uint8_t>(packet[0] & 0xF0u);

    std::size_t remaining = 0;
    const int headerBytes = readRemainingLength(packet + 1, length - 1, remaining);
    if (headerBytes <= 0) {
        return;
    }
    const std::uint8_t* body = packet + 1 + headerBytes;

    switch (type) {
        case kConnack: {
            if (remaining < 2) {
                fail("short connack");
                return;
            }
            // body[1] is the return code. Anything but zero is a refusal, and
            // the broker closes the socket itself immediately afterwards.
            if (body[1] != 0) {
                fail("connection refused");
                return;
            }
            awaitingConnack_ = false;
            setState(MqttState::Connected);
            return;
        }

        case kPublish: {
            const bool qos1 = ((packet[0] >> 1) & 0x03u) == 1;
            if (remaining < 2) {
                return;
            }
            const std::size_t topicLength =
                (static_cast<std::size_t>(body[0]) << 8) | body[1];
            std::size_t at = 2 + topicLength;
            if (at > remaining) {
                return;
            }

            MqttMessage message;
            message.topic.assign(reinterpret_cast<const char*>(body + 2), topicLength);
            message.retained = (packet[0] & 0x01u) != 0;
            message.qos = qos1 ? 1 : 0;

            std::uint16_t packetId = 0;
            if (qos1) {
                if (at + 2 > remaining) {
                    return;
                }
                packetId = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(body[at]) << 8) | body[at + 1]);
                at += 2;
            }

            message.payload.assign(reinterpret_cast<const char*>(body + at),
                                   remaining - at);

            if (qos1) {
                std::string ack;
                ack.push_back(static_cast<char>(kPuback));
                appendRemainingLength(ack, 2);
                appendUint16(ack, packetId);
                enqueue(ack);
            }

            if (listener_ != nullptr) {
                listener_->onMessage(message);
            }
            return;
        }

        // Acknowledgements with nothing further owed. Arriving at all is what
        // proves the link is alive, and pumpReads has already stamped the
        // activity clock that the keepalive reads.
        case kPuback:
        case kSuback:
        case kPingresp:
        default:
            return;
    }
}

void Tc002MqttClient::pumpReads(std::uint64_t nowMillis) {
    for (;;) {
        char chunk[1024];
        const ssize_t got = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (got > 0) {
            if (inbound_.size() + static_cast<std::size_t>(got) > kMaxPacketBytes) {
                fail("oversized packet");
                return;
            }
            inbound_.append(chunk, static_cast<std::size_t>(got));
            lastActivityMillis_ = nowMillis;
            continue;
        }
        if (got == 0) {
            fail("broker closed the connection");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        fail("recv");
        return;
    }

    // Drain whole packets; a partial one simply waits for the next poll.
    for (;;) {
        if (inbound_.size() < 2) {
            return;
        }
        const auto* data = reinterpret_cast<const std::uint8_t*>(inbound_.data());

        std::size_t remaining = 0;
        const int headerBytes =
            readRemainingLength(data + 1, inbound_.size() - 1, remaining);
        if (headerBytes == 0) {
            return;  // length still arriving
        }
        if (headerBytes < 0 || remaining > kMaxPacketBytes) {
            fail("malformed length");
            return;
        }

        const std::size_t total = 1 + static_cast<std::size_t>(headerBytes) + remaining;
        if (inbound_.size() < total) {
            return;  // body still arriving
        }

        handlePacket(data, total);
        if (fd_ < 0) {
            return;  // handlePacket closed us
        }
        inbound_.erase(0, total);
    }
}

void Tc002MqttClient::poll(std::uint64_t nowMillis) {
    if (lastActivityMillis_ == 0) {
        lastActivityMillis_ = nowMillis;
    }
    if (fd_ < 0) {
        return;
    }

    if (state_ == MqttState::Connecting && !awaitingConnack_) {
        if (connectStartedMillis_ == 0) {
            connectStartedMillis_ = nowMillis;
        }
        if (nowMillis - connectStartedMillis_ > kConnectTimeoutMillis) {
            fail("connect timed out");
            return;
        }
        if (!finishConnect()) {
            // Either still in progress or refused; getsockopt cannot tell those
            // apart without waiting, so the timeout above is what ends it.
            return;
        }
        sendConnectPacket();
    }

    pumpWrites();
    if (fd_ < 0) {
        return;
    }

    pumpReads(nowMillis);
    if (fd_ < 0) {
        return;
    }

    // Keepalive at half the interval, so a lost PINGRESP still leaves room for
    // a second attempt before the broker gives up on us.
    if (state_ == MqttState::Connected && options_.keepAliveSeconds > 0) {
        const std::uint64_t half =
            static_cast<std::uint64_t>(options_.keepAliveSeconds) * 500u;
        if (nowMillis - lastActivityMillis_ >= half) {
            std::string ping;
            ping.push_back(static_cast<char>(kPingreq));
            ping.push_back(0);
            enqueue(ping);
            lastActivityMillis_ = nowMillis;
            pumpWrites();
        }
    }
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
