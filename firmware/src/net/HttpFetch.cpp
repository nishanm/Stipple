// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/net/HttpFetch.h"

#include <cstdlib>

namespace stipple {
namespace net {
namespace http {
namespace {

bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
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

/// A host a script may name.
///
/// Letters, digits, dash and dot. No underscores, no brackets: rejecting an
/// IPv6 literal here is honest, because nothing below this can connect to one
/// anyway, and accepting it would fail later and less clearly.
bool plausibleHost(std::string_view host) noexcept {
    if (host.empty() || host.size() > 128) {
        return false;
    }
    if (host.front() == '-' || host.front() == '.' || host.back() == '.') {
        return false;
    }
    for (const char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        digit(c) || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool parseUrl(std::string_view url, Url& out) noexcept {
    if (url.empty() || url.size() > kMaxUrlBytes) {
        return false;
    }

    out = Url{};

    if (url.size() > 7 && equalsIgnoringCase(url.substr(0, 7), "http://")) {
        url.remove_prefix(7);
        out.secure = false;
        out.port = 80;
    } else if (url.size() > 8 && equalsIgnoringCase(url.substr(0, 8), "https://")) {
        url.remove_prefix(8);
        out.secure = true;
        out.port = 443;
    } else {
        // No scheme guessing. "example.com/x" could be a host or a path, and a
        // parser that decides is one that can be surprised.
        return false;
    }

    // Credentials are refused rather than ignored. A URL carrying a password
    // ends up in a script somebody pastes onto a public page, and silently
    // dropping the credentials would produce a 401 nobody could explain.
    const std::size_t authorityEnd = url.find_first_of("/?#");
    const std::string_view authority =
        authorityEnd == std::string_view::npos ? url : url.substr(0, authorityEnd);
    if (authority.find('@') != std::string_view::npos) {
        return false;
    }

    std::string_view host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        const std::string_view digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) {
            return false;
        }
        int port = 0;
        for (const char c : digits) {
            if (!digit(c)) {
                return false;
            }
            port = port * 10 + (c - '0');
        }
        if (port <= 0 || port > 65535) {
            return false;
        }
        out.port = port;
    }

    if (!plausibleHost(host)) {
        return false;
    }
    out.host.assign(host);

    if (authorityEnd == std::string_view::npos) {
        out.target = "/";
        return true;
    }

    std::string_view rest = url.substr(authorityEnd);
    // A fragment never reaches the server, so it is dropped here rather than
    // sent and ignored.
    const std::size_t hash = rest.find('#');
    if (hash != std::string_view::npos) {
        rest = rest.substr(0, hash);
    }
    if (rest.empty() || rest.front() == '?') {
        out.target = "/";
        out.target.append(rest);
    } else {
        out.target.assign(rest);
    }

    // A control character in the target would split the request line and let
    // a script smuggle a second request onto the connection. This is the one
    // check in the parser that is a security boundary rather than a
    // convenience.
    for (const char c : out.target) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F) {
            return false;
        }
    }
    return true;
}

bool headerIsSafe(std::string_view name, std::string_view value) noexcept {
    if (name.empty()) {
        return true;
    }
    for (const char c : name) {
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool mark = c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
                          c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
                          c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!alpha && !digit(c) && !mark) {
            return false;
        }
    }
    for (const char c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

std::string buildGet(const Url& url, std::string_view userAgent, std::string_view headerName,
                     std::string_view headerValue) {
    std::string request;
    request.reserve(url.target.size() + url.host.size() + userAgent.size() + headerName.size() +
                    headerValue.size() + 100);
    request += "GET ";
    request += url.target;
    request += " HTTP/1.1\r\nHost: ";
    request += url.host;
    // The port only appears when it is not the default. Sending "Host:
    // example.com:80" is legal and confuses enough virtual-host setups to be
    // worth avoiding.
    if ((!url.secure && url.port != 80) || (url.secure && url.port != 443)) {
        request += ':';
        request += std::to_string(url.port);
    }
    request += "\r\nUser-Agent: ";
    request.append(userAgent);
    if (!headerName.empty()) {
        request += "\r\n";
        request.append(headerName);
        request += ": ";
        request.append(headerValue);
    }
    // No compression offered. Decompressing would mean carrying zlib for a
    // device that reads a kilobyte every few minutes.
    request += "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    return request;
}

// --- the parser --------------------------------------------------------------

void ResponseParser::reset() noexcept {
    stage_ = Stage::Status;
    pending_.clear();
    failure_.clear();
    response_ = Response{};
    headerBytes_ = 0;
    contentLength_ = -1;
    bodyTaken_ = 0;
}

bool ResponseParser::done() const noexcept {
    return stage_ == Stage::Complete;
}

bool ResponseParser::fail(const char* why) {
    stage_ = Stage::Broken;
    if (failure_.empty()) {
        failure_ = why;
    }
    return false;
}

bool ResponseParser::consumeStatusLine(std::string_view line) {
    // "HTTP/1.1 200 OK". The reason phrase is thrown away: nothing reads it,
    // and a server is free to put anything there.
    if (line.size() < 12 || !equalsIgnoringCase(line.substr(0, 5), "http/")) {
        return fail("not an HTTP response");
    }
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || space + 4 > line.size()) {
        return fail("malformed status line");
    }
    int status = 0;
    for (std::size_t i = space + 1; i < space + 4; ++i) {
        if (!digit(line[i])) {
            return fail("malformed status code");
        }
        status = status * 10 + (line[i] - '0');
    }
    response_.status = status;
    stage_ = Stage::Headers;
    return true;
}

bool ResponseParser::consumeHeader(std::string_view line) {
    if (line.empty() || line == "\r") {
        // End of headers. A 204 or a 304 has no body whatever the headers say,
        // and waiting for one would hang until the server closed.
        if (response_.status == 204 || response_.status == 304) {
            stage_ = Stage::Complete;
            response_.complete = true;
            return true;
        }
        stage_ = response_.chunked ? Stage::Chunk : Stage::Body;
        return true;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return fail("malformed header");
    }
    const std::string_view name = trim(line.substr(0, colon));
    const std::string_view value = trim(line.substr(colon + 1));

    if (equalsIgnoringCase(name, "content-length")) {
        long long length = 0;
        if (value.empty()) {
            return fail("empty content-length");
        }
        for (const char c : value) {
            if (!digit(c)) {
                return fail("malformed content-length");
            }
            length = length * 10 + (c - '0');
            if (length > (1LL << 40)) {
                return fail("absurd content-length");
            }
        }
        contentLength_ = length;
    } else if (equalsIgnoringCase(name, "transfer-encoding")) {
        if (equalsIgnoringCase(value, "chunked")) {
            response_.chunked = true;
            // Chunked wins. A response carrying both is malformed, and the
            // standard says to prefer the framing that cannot be desynchronised
            // by a proxy.
            contentLength_ = -1;
        }
    }
    return true;
}

bool ResponseParser::consumeBody() {
    const std::size_t room = maxBodyBytes_ > response_.body.size()
                                 ? maxBodyBytes_ - response_.body.size()
                                 : 0;
    const std::size_t take = pending_.size() < room ? pending_.size() : room;
    response_.body.append(pending_, 0, take);
    if (take < pending_.size()) {
        response_.truncated = true;
    }
    bodyTaken_ += pending_.size();
    pending_.clear();

    if (contentLength_ >= 0 &&
        bodyTaken_ >= static_cast<std::size_t>(contentLength_)) {
        stage_ = Stage::Complete;
        response_.complete = true;
    } else if (response_.truncated && contentLength_ < 0) {
        // Nothing more will be kept and there is no length to wait for, so
        // waiting for the server to close would be waiting for nothing.
        stage_ = Stage::Complete;
        response_.complete = true;
    }
    return true;
}

bool ResponseParser::consumeChunks() {
    for (;;) {
        const std::size_t eol = pending_.find("\r\n");
        if (eol == std::string::npos) {
            if (pending_.size() > 64) {
                return fail("malformed chunk size");
            }
            return true;  // wait for more
        }

        // "1a2b" or "1a2b;ext=1". Everything after the semicolon is ignored.
        std::string_view header(pending_.data(), eol);
        const std::size_t semi = header.find(';');
        if (semi != std::string_view::npos) {
            header = header.substr(0, semi);
        }
        header = trim(header);
        if (header.empty() || header.size() > 8) {
            return fail("malformed chunk size");
        }

        std::size_t size = 0;
        for (const char c : header) {
            int value;
            if (digit(c)) {
                value = c - '0';
            } else if (lower(c) >= 'a' && lower(c) <= 'f') {
                value = lower(c) - 'a' + 10;
            } else {
                return fail("malformed chunk size");
            }
            size = size * 16 + static_cast<std::size_t>(value);
        }

        if (size == 0) {
            // The trailer is not waited for. Nothing here reads trailers, and
            // a server that never sends the final CRLF would otherwise hold
            // the fetch open until its timeout.
            stage_ = Stage::Complete;
            response_.complete = true;
            pending_.clear();
            return true;
        }

        // The chunk plus its trailing CRLF.
        if (pending_.size() < eol + 2 + size + 2) {
            return true;  // wait for more
        }

        const std::size_t room = maxBodyBytes_ > response_.body.size()
                                     ? maxBodyBytes_ - response_.body.size()
                                     : 0;
        const std::size_t take = size < room ? size : room;
        response_.body.append(pending_, eol + 2, take);
        if (take < size) {
            response_.truncated = true;
        }
        pending_.erase(0, eol + 2 + size + 2);

        if (response_.truncated) {
            stage_ = Stage::Complete;
            response_.complete = true;
            pending_.clear();
            return true;
        }
    }
}

bool ResponseParser::feed(std::string_view bytes) {
    if (stage_ == Stage::Broken) {
        return false;
    }
    if (stage_ == Stage::Complete) {
        return true;  // extra bytes after the body are simply ignored
    }

    pending_.append(bytes);

    while (stage_ == Stage::Status || stage_ == Stage::Headers) {
        const std::size_t eol = pending_.find('\n');
        if (eol == std::string::npos) {
            headerBytes_ += bytes.size();
            if (headerBytes_ > kMaxHeaderBytes) {
                return fail("headers too long");
            }
            return true;
        }

        std::string_view line(pending_.data(), eol);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        headerBytes_ += eol + 1;
        if (headerBytes_ > kMaxHeaderBytes) {
            return fail("headers too long");
        }

        const bool ok = stage_ == Stage::Status ? consumeStatusLine(line)
                                                : consumeHeader(line);
        pending_.erase(0, eol + 1);
        if (!ok) {
            return false;
        }
    }

    if (stage_ == Stage::Body) {
        return consumeBody();
    }
    if (stage_ == Stage::Chunk) {
        return consumeChunks();
    }
    return true;
}

}  // namespace http
}  // namespace net
}  // namespace stipple
