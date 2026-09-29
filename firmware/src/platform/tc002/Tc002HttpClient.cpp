// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002HttpClient.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "stipple/core/Version.h"
#include "stipple/net/DnsMessage.h"
#include "stipple/platform/tc002/Tc002Tls.h"
#include "stipple/net/HttpFetch.h"

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

using net::http::ResponseParser;
using net::http::Url;

/// Closes on the way out of any path, including the ones that return early.
class Socket {
public:
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

/// How long one nameserver gets before the next is tried.
///
/// Two seconds, against glibc's five-with-two-attempts. That difference is
/// the whole reason this does not call `getaddrinfo`: the TC002 ships
/// `nameserver 114.114.114.114` in its resolv.conf, which is unreachable
/// from most of the world, so glibc spends over ten seconds on a dead server
/// before trying the one that works. Measured on the device - a plain
/// `http://example.com/` fetch hit the fetcher's twenty-second backstop while
/// `http://1.1.1.1/` answered 301 immediately.
constexpr int kDnsTimeoutSeconds = 2;

/// The nameserver that answered last, tried first next time.
///
/// Saves paying the dead-server timeout on every single fetch once something
/// has worked. Plain int rather than an atomic: a torn read costs one
/// needless timeout and cannot do anything worse, and only one fetch runs at
/// a time anyway.
int g_preferredNameserver = 0;

/// Ask one nameserver for an A record. False on anything that is not an
/// answer, including a timeout.
bool askNameserver(std::uint32_t server, const std::string& host,
                   std::uint32_t& address) {
    Socket socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!socket.valid()) {
        return false;
    }

    struct timeval patience;
    patience.tv_sec = kDnsTimeoutSeconds;
    patience.tv_usec = 0;
    ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));

    // Unpredictable rather than sequential, as DnsMessage::build asks: a
    // resolver that counts 1, 2, 3 is one an off-path attacker can answer
    // before the real server does.
    static std::mt19937 generator{std::random_device{}()};
    const auto id = static_cast<std::uint16_t>(
        std::uniform_int_distribution<int>(1, 0xFFFF)(generator));

    std::uint8_t query[net::dns::kMaxMessageBytes];
    const std::size_t length = net::dns::build(host, id, query, sizeof(query));
    if (length == 0) {
        return false;
    }

    struct sockaddr_in to;
    std::memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(server);

    if (::sendto(socket.get(), query, length, 0,
                 reinterpret_cast<struct sockaddr*>(&to), sizeof(to)) < 0) {
        return false;
    }

    std::uint8_t reply[net::dns::kMaxMessageBytes];
    const ssize_t got = ::recv(socket.get(), reply, sizeof(reply), 0);
    if (got <= 0) {
        return false;
    }

    return net::dns::parse(reply, static_cast<std::size_t>(got), id, address) ==
           net::dns::Result::Ok;
}

/// One address for a host.
///
/// A dotted quad needs nothing. A name is resolved by asking the nameservers
/// in resolv.conf ourselves, with a short timeout each, rather than through
/// `getaddrinfo` - see kDnsTimeoutSeconds for why.
bool resolve(const std::string& host, int port, struct sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) {
        return true;
    }

    std::string conf;
    if (std::FILE* file = std::fopen("/etc/resolv.conf", "rb"); file != nullptr) {
        char buffer[1024];
        std::size_t read;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            conf.append(buffer, read);
            if (conf.size() > 8192) {
                break;  // bounded like everything else read from outside
            }
        }
        std::fclose(file);
    }

    std::uint32_t servers[net::dns::kMaxNameservers];
    std::size_t count = net::dns::parseNameservers(conf, servers, net::dns::kMaxNameservers);
    if (count == 0) {
        // A device with no resolv.conf is not a device with no network. This
        // is a guess, and it is a better one than failing outright - but it
        // is the only guess here, and it is deliberately a public resolver
        // rather than a router address that would only work on one network.
        servers[0] = 0x08080808u;  // 8.8.8.8
        count = 1;
    }

    std::uint32_t address = 0;
    for (std::size_t attempt = 0; attempt < count; ++attempt) {
        const std::size_t index =
            (static_cast<std::size_t>(g_preferredNameserver) + attempt) % count;
        if (askNameserver(servers[index], host, address)) {
            g_preferredNameserver = static_cast<int>(index);
            out.sin_addr.s_addr = address;
            return true;
        }
    }
    return false;
}

/// Connect, but give up after `seconds` rather than after the kernel's own
/// patience - which on an unreachable address is about two minutes, and would
/// hold this feed's slot for six times the fetcher's backstop.
bool connectWithin(int fd, const struct sockaddr_in& address, int seconds) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    if (::connect(fd, reinterpret_cast<const struct sockaddr*>(&address),
                  sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            return false;
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval patience;
        patience.tv_sec = seconds;
        patience.tv_usec = 0;
        if (::select(fd + 1, nullptr, &writable, nullptr, &patience) <= 0) {
            return false;
        }
        // select() says writable for a refused connection too, so the error
        // has to be read back explicitly. Without this, a closed port looks
        // like a successful connect and fails later with something
        // unhelpful.
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            return false;
        }
    }

    // Back to blocking, with timeouts, for the send and the read. The rest of
    // the exchange is a straight line and does not need a state machine.
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

/// The socket, or the socket with TLS over it.
///
/// One type for both so the request/response loop below is written once. It
/// was two copies for about ten minutes and the plaintext one immediately
/// drifted - which is how a security fix lands in one path and not the other.
class Transport {
public:
    Transport(int fd, void* session) noexcept : fd_(fd), session_(session) {}
    ~Transport() {
        if (session_ != nullptr) {
            Tls::instance().close(session_);
        }
    }

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool sendAll(const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const std::size_t left = bytes.size() - sent;
            int wrote;
            if (session_ != nullptr) {
                wrote = Tls::instance().write(session_, bytes.data() + sent,
                                              static_cast<int>(left));
            } else {
                const ssize_t n = ::send(fd_, bytes.data() + sent, left, MSG_NOSIGNAL);
                wrote = static_cast<int>(n);
            }
            if (wrote <= 0) {
                if (wrote < 0 && session_ == nullptr && errno == EINTR) {
                    continue;
                }
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    /// Negative on error, zero at end of stream.
    int receive(char* buffer, int length) {
        if (session_ != nullptr) {
            return Tls::instance().read(session_, buffer, length);
        }
        return static_cast<int>(::recv(fd_, buffer, static_cast<std::size_t>(length), 0));
    }

    bool secure() const noexcept { return session_ != nullptr; }

private:
    int fd_;
    void* session_;
};

}  // namespace

Tc002HttpClient::~Tc002HttpClient() {
    // The worker is detached and owns its half of the exchange through a
    // shared_ptr, so it can safely outlive this object. Nothing to wait for.
}

bool Tc002HttpClient::begin(const HttpRequest& request) {
    if (stage_ == Stage::Running) {
        return false;
    }

    Url parsed;
    if (!net::http::parseUrl(request.url, parsed)) {
        stage_ = Stage::Failed;
        failure_ = "bad url";
        return false;
    }
    if (!net::http::headerIsSafe(request.headerName, request.headerValue)) {
        stage_ = Stage::Failed;
        failure_ = "bad header";
        return false;
    }

    // https is refused *here* only when this device cannot do it safely -
    // no OpenSSL, or no trusted roots to check a certificate against. Never
    // downgraded to plaintext: fetching over http what somebody asked to
    // fetch over https would put their API key on the wire of a network they
    // believed was protected, which is the same call Tc002MqttClient makes
    // about a broker password.
    if (parsed.secure && !Tls::instance().usable()) {
        stage_ = Stage::Failed;
        failure_.assign(Tls::instance().problem());
        return false;
    }

    auto exchange = std::make_shared<Exchange>();
    exchange_ = exchange;

    // kVersion is a string_view, and C++17 has no operator+ for
    // string + string_view - appending is the whole conversion.
    std::string agent = "Stipple/";
    agent.append(kVersion);
    const std::string wire =
        net::http::buildGet(parsed, agent, request.headerName, request.headerValue);
    const std::string host = parsed.host;
    const int port = parsed.port;
    const bool secure = parsed.secure;
    const std::size_t maxBody = request.maxBodyBytes;
    // Headers count towards the transfer cap, so a request that asked for a
    // larger body gets the same headroom above it the default one has.
    const std::size_t transferCap =
        maxBody + ResponseParser::kMaxHeaderBytes > kMaxTransferBytes
            ? maxBody + ResponseParser::kMaxHeaderBytes
            : kMaxTransferBytes;

    try {
        std::thread worker([exchange, host, port, wire, secure, maxBody, transferCap]() {
            struct sockaddr_in address;
            if (!resolve(host, port, address)) {
                exchange->failure = "cannot resolve " + host;
                exchange->done.store(true);
                return;
            }

            Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
            if (!socket.valid()) {
                exchange->failure = "no socket";
                exchange->done.store(true);
                return;
            }

            struct timeval patience;
            patience.tv_sec = kTimeoutSeconds;
            patience.tv_usec = 0;
            ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
            ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof(patience));
            const int nodelay = 1;
            ::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            if (!connectWithin(socket.get(), address, kTimeoutSeconds)) {
                exchange->failure = "cannot reach " + host;
                exchange->done.store(true);
                return;
            }

            // The handshake, for an https URL. Everything that decides
            // whether this connection is trustworthy happens in here: chain,
            // hostname, protocol floor. A failure names the reason.
            void* session = nullptr;
            if (secure) {
                std::string why;
                session = Tls::instance().connect(socket.get(), host, why);
                if (session == nullptr) {
                    exchange->failure = why.empty() ? "tls failed" : why;
                    exchange->done.store(true);
                    return;
                }
            }
            Transport transport(socket.get(), session);

            if (!transport.sendAll(wire)) {
                exchange->failure = "send failed";
                exchange->done.store(true);
                return;
            }

            ResponseParser parser(maxBody);
            char chunk[2048];
            std::size_t total = 0;

            for (;;) {
                const int got = transport.receive(chunk, static_cast<int>(sizeof(chunk)));
                if (got < 0) {
                    if (!transport.secure() && errno == EINTR) {
                        continue;
                    }
                    exchange->failure = "read timed out";
                    break;
                }
                if (got == 0) {
                    // The server closed. With Connection: close that is the
                    // end of a body with no Content-Length, which is a
                    // perfectly ordinary way for a response to end.
                    break;
                }

                total += static_cast<std::size_t>(got);
                if (!parser.feed(std::string_view(chunk, static_cast<std::size_t>(got)))) {
                    exchange->failure.assign(parser.failure());
                    break;
                }
                if (parser.done()) {
                    break;
                }
                if (total > transferCap) {
                    // Everything worth keeping is already kept; the rest is a
                    // server talking to itself.
                    break;
                }
            }

            if (exchange->failure.empty()) {
                const net::http::Response& response = parser.response();
                if (response.status == 0) {
                    exchange->failure = "no response";
                } else {
                    exchange->status.store(response.status);
                    exchange->body = response.body;
                    exchange->ok.store(true);
                }
            }
            // Written last, and the only thing the other side polls. Every
            // field above is published by this store.
            exchange->done.store(true);
        });
        worker.detach();
    } catch (...) {
        // A thread that will not start must not take the panel with it.
        exchange_.reset();
        stage_ = Stage::Failed;
        failure_ = "cannot start fetch";
        return false;
    }

    stage_ = Stage::Running;
    failure_.clear();
    status_ = 0;
    body_.clear();
    return true;
}

void Tc002HttpClient::poll(std::uint64_t) {
    if (stage_ != Stage::Running || exchange_ == nullptr) {
        return;
    }
    if (!exchange_->done.load()) {
        return;
    }

    if (exchange_->ok.load()) {
        status_ = exchange_->status.load();
        body_ = exchange_->body;
        failure_.clear();
        stage_ = Stage::Done;
    } else {
        failure_ = exchange_->failure.empty() ? "fetch failed" : exchange_->failure;
        status_ = 0;
        body_.clear();
        stage_ = Stage::Failed;
    }
    exchange_.reset();
}

void Tc002HttpClient::reset() {
    // A worker still running is left to finish into its own shared block and
    // drop it. Detaching the result here rather than waiting is what keeps
    // reset() instant, which matters because the fetcher calls it from the
    // frame loop.
    exchange_.reset();
    stage_ = Stage::Idle;
    status_ = 0;
    body_.clear();
    failure_.clear();
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
