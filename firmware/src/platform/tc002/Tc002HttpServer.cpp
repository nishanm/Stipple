// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002HttpServer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

constexpr std::size_t kReadChunk = 2048;

/// Reason phrases for the statuses this device actually returns.
///
/// A fixed list rather than a generic table, for the same reason matchRoute is
/// a list of paths: the set is small, it is public API surface, and having it
/// visible in one place is worth more than covering codes we never emit.
const char* reasonPhrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        default: return "Error";
    }
}

bool setNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char lhs = a[i];
        char rhs = b[i];
        if (lhs >= 'A' && lhs <= 'Z') { lhs = static_cast<char>(lhs + 32); }
        if (rhs >= 'A' && rhs <= 'Z') { rhs = static_cast<char>(rhs + 32); }
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/// api::Request documents its path as already percent-decoded, so the transport
/// owes it that. A malformed escape is left as written rather than guessed at.
std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

}  // namespace

Tc002HttpServer::~Tc002HttpServer() { stop(); }

bool Tc002HttpServer::start(int port, IHttpRequestHandler& handler) {
    stop();

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        return false;
    }

    // Without this a restart within the TIME_WAIT window cannot rebind, which
    // during development is every restart.
    const int reuse = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (!setNonBlocking(listenFd_)) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    // INADDR_ANY: a clock whose config page is only reachable from itself is
    // useless. §23's "no default internet exposure" is about routers and
    // authentication, not about refusing the LAN this device exists to serve.
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&address),
               sizeof(address)) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::listen(listenFd_, kListenBacklog) != 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    handler_ = &handler;
    port_ = port;
    return true;
}

void Tc002HttpServer::stop() {
    for (Connection& connection : connections_) {
        closeConnection(connection);
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    handler_ = nullptr;
    port_ = 0;
}

bool Tc002HttpServer::isUpload(const std::string& inbound) noexcept {
    const std::size_t prefix = std::strlen(kUploadRequestLine);
    return inbound.size() >= prefix && inbound.compare(0, prefix, kUploadRequestLine) == 0;
}

std::size_t Tc002HttpServer::ceilingFor(const Connection& connection) const {
    // Decided from the first line, which arrives in the first packet. Until
    // enough has arrived to tell, the small ceiling applies - and it is far
    // larger than a request line, so nothing is ever refused for being
    // undecidable.
    if (!isUpload(connection.inbound)) {
        return kMaxRequestBytes;
    }
    // One upload at a time. The one that got past the small ceiling first
    // keeps the large one; any other is held to the small ceiling.
    for (const Connection& other : connections_) {
        if (&other != &connection && other.fd >= 0 && isUpload(other.inbound) &&
            other.inbound.size() > kMaxRequestBytes) {
            return kMaxRequestBytes;
        }
    }
    return kMaxUploadBytes;
}

bool Tc002HttpServer::hasFreeSlot() const noexcept {
    for (const Connection& connection : connections_) {
        if (connection.fd < 0) {
            return true;
        }
    }
    return false;
}

void Tc002HttpServer::closeConnection(Connection& connection) noexcept {
    if (connection.fd >= 0) {
        ::close(connection.fd);
        connection.fd = -1;
    }
    connection.inbound.clear();
    connection.outbound.clear();
    connection.sent = 0;
    connection.closing = false;
}

void Tc002HttpServer::acceptPending(std::uint64_t nowMillis) {
    for (;;) {
        // A full table leaves the rest in the listen backlog, where the kernel
        // holds them until a slot frees on a later poll. Accepting one only to
        // close it turned a burst of six requests into lost answers; the
        // backlog is the bound now, and past it the kernel makes the client
        // retry its SYN rather than seeing a connection reset.
        if (!hasFreeSlot()) {
            return;
        }

        const int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            return;  // EAGAIN: nothing waiting, which is the usual case.
        }

        Connection* slot = nullptr;
        for (Connection& connection : connections_) {
            if (connection.fd < 0) {
                slot = &connection;
                break;
            }
        }

        if (slot == nullptr) {
            // Cannot happen after hasFreeSlot(), kept so a change there cannot
            // leak a descriptor.
            ::close(fd);
            ++rejected_;
            return;
        }

        if (!setNonBlocking(fd)) {
            ::close(fd);
            continue;
        }

        slot->fd = fd;
        slot->inbound.clear();
        slot->outbound.clear();
        slot->sent = 0;
        slot->closing = false;
        slot->lastProgressMillis = nowMillis;
    }
}

bool Tc002HttpServer::tryParse(const std::string& raw, api::Request& request,
                               bool& malformed) const {
    malformed = false;

    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;  // headers still arriving
    }

    const std::string_view head(raw.data(), headerEnd);

    const std::size_t requestLineEnd = head.find("\r\n");
    if (requestLineEnd == std::string_view::npos) {
        malformed = true;
        return true;
    }

    // --- request line: METHOD SP TARGET SP VERSION ---------------------------
    const std::string_view requestLine = head.substr(0, requestLineEnd);

    const std::size_t firstSpace = requestLine.find(' ');
    if (firstSpace == std::string_view::npos) {
        malformed = true;
        return true;
    }
    const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) {
        malformed = true;
        return true;
    }

    request.method = api::methodFromName(requestLine.substr(0, firstSpace));

    std::string_view target =
        requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    const std::size_t queryStart = target.find('?');
    if (queryStart == std::string_view::npos) {
        request.path = percentDecode(target);
        request.query.clear();
    } else {
        request.path = percentDecode(target.substr(0, queryStart));
        // The query keeps its escaping: queryValue() owns decoding a value, and
        // decoding here would make a '&' inside a value indistinguishable from
        // a separator.
        request.query = std::string(target.substr(queryStart + 1));
    }

    // --- headers -------------------------------------------------------------
    std::size_t contentLength = 0;
    bool haveContentLength = false;

    std::size_t cursor = requestLineEnd + 2;
    while (cursor < head.size()) {
        std::size_t lineEnd = head.find("\r\n", cursor);
        if (lineEnd == std::string_view::npos) {
            lineEnd = head.size();
        }

        const std::string_view line = head.substr(cursor, lineEnd - cursor);
        cursor = lineEnd + 2;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }

        const std::string_view name = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));

        if (equalsIgnoreCase(name, "content-length")) {
            std::size_t parsed = 0;
            bool digits = !value.empty();
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
                if (parsed > kMaxUploadBytes) {
                    // The wider bound here, because the narrower one is
                    // enforced on accumulation where the request line is
                    // known. A Content-Length check cannot see the path.
                    malformed = true;
                    return true;
                }
            }
            if (!digits) {
                malformed = true;
                return true;
            }
            contentLength = parsed;
            haveContentLength = true;
        } else if (equalsIgnoreCase(name, "if-none-match")) {
            request.ifNoneMatch = std::string(value);
        } else if (equalsIgnoreCase(name, "x-api-key")) {
            request.authToken = std::string(value);
        } else if (equalsIgnoreCase(name, "authorization")) {
            // Kept whole as well as split. Basic is decided in the core, and
            // the transport's job is to hand over what arrived rather than
            // to decide what it means.
            request.authorization = std::string(value);

            constexpr std::string_view kBearer = "Bearer ";
            if (value.size() > kBearer.size() &&
                equalsIgnoreCase(value.substr(0, kBearer.size()), kBearer)) {
                request.authToken = std::string(trim(value.substr(kBearer.size())));
            }
        }
    }

    const std::size_t bodyStart = headerEnd + 4;
    const std::size_t available = raw.size() - bodyStart;

    if (haveContentLength && available < contentLength) {
        return false;  // body still arriving
    }

    if (haveContentLength && contentLength > 0) {
        request.body.assign(raw, bodyStart, contentLength);
    }

    return true;
}

void Tc002HttpServer::queueResponse(Connection& connection,
                                    const api::Response& response) const {
    char header[512];

    // Connection: close, deliberately. Keep-alive would mean tracking request
    // boundaries across polls for a device that serves a handful of requests a
    // minute; the complexity buys nothing here and every HTTP client handles a
    // close correctly.
    int written = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n",
        response.status, reasonPhrase(response.status),
        response.contentType.c_str(), response.body.size());

    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(header)) {
        connection.outbound = "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
        connection.sent = 0;
        connection.closing = true;
        return;
    }

    connection.outbound.assign(header, static_cast<std::size_t>(written));

    if (!response.etag.empty()) {
        connection.outbound += "ETag: " + response.etag + "\r\n";
    }
    if (!response.wwwAuthenticate.empty()) {
        connection.outbound += "WWW-Authenticate: " + response.wwwAuthenticate + "\r\n";
    }
    if (!response.cacheControl.empty()) {
        connection.outbound += "Cache-Control: " + response.cacheControl + "\r\n";
    }

    connection.outbound += "\r\n";
    connection.outbound += response.body;
    connection.sent = 0;
    connection.closing = true;
}

void Tc002HttpServer::service(Connection& connection, std::uint64_t nowMillis) {
    // --- write pending output ------------------------------------------------
    if (!connection.outbound.empty()) {
        while (connection.sent < connection.outbound.size()) {
            const ssize_t wrote =
                ::send(connection.fd, connection.outbound.data() + connection.sent,
                       connection.outbound.size() - connection.sent, MSG_NOSIGNAL);
            if (wrote > 0) {
                connection.sent += static_cast<std::size_t>(wrote);
                connection.lastProgressMillis = nowMillis;
                continue;
            }
            if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;  // socket full; finish on a later poll
            }
            closeConnection(connection);
            return;
        }

        closeConnection(connection);
        return;
    }

    // --- read what has arrived -----------------------------------------------
    const std::size_t ceiling = ceilingFor(connection);
    char chunk[kReadChunk];
    for (;;) {
        const ssize_t got = ::recv(connection.fd, chunk, sizeof(chunk), 0);
        if (got > 0) {
            if (connection.inbound.size() + static_cast<std::size_t>(got) >
                ceilingFor(connection)) {
                ++rejected_;
                queueResponse(connection, api::payloadTooLarge(
                                              ceiling > kMaxRequestBytes
                                                  ? "image exceeds 4 MiB"
                                                  : "request exceeds 64 KiB"));
                return;
            }
            connection.inbound.append(chunk, static_cast<std::size_t>(got));
            connection.lastProgressMillis = nowMillis;
            continue;
        }

        if (got == 0) {
            // Peer closed before finishing. Nothing to answer.
            closeConnection(connection);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        closeConnection(connection);
        return;
    }

    if (connection.inbound.empty()) {
        return;
    }

    api::Request request;
    bool malformed = false;
    if (!tryParse(connection.inbound, request, malformed)) {
        return;  // incomplete; wait for more
    }

    if (malformed) {
        ++rejected_;
        queueResponse(connection, api::badRequest("malformed request"));
        return;
    }

    ++served_;
    queueResponse(connection, handler_->handle(request));
}

void Tc002HttpServer::poll(std::uint64_t nowMillis) {
    if (listenFd_ < 0 || handler_ == nullptr) {
        return;
    }

    acceptPending(nowMillis);

    for (Connection& connection : connections_) {
        if (connection.fd < 0) {
            continue;
        }

        service(connection, nowMillis);

        // Checked after servicing, so a connection that just made progress is
        // not closed on a stale timestamp.
        if (connection.fd >= 0 &&
            nowMillis - connection.lastProgressMillis > kIdleTimeoutMillis) {
            ++rejected_;
            closeConnection(connection);
        }
    }
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
