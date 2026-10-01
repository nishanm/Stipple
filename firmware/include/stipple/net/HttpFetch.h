// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stipple {
namespace net {
namespace http {

/// A URL, taken apart.
///
/// Only what a fetch needs. No user-info, no fragment: a fragment never
/// reaches a server, and credentials in a URL are a way to put a password in
/// a script somebody pastes onto a public page.
struct Url {
    bool secure = false;  ///< https
    std::string host;
    int port = 80;
    std::string target;  ///< path plus query, starting with '/'
};

/// Longest URL a script may ask for.
///
/// Bounded like everything else that arrives from the network — and a script
/// building a URL in a loop is a real way to run a device out of memory
/// slowly enough that nobody connects the two.
inline constexpr std::size_t kMaxUrlBytes = 256;

/// Take a URL apart. False when it is not one this device will fetch.
///
/// Deliberately strict. This string comes from a script, and a parser that
/// guesses at a malformed URL is one that can be talked into connecting
/// somewhere its author did not mean.
bool parseUrl(std::string_view url, Url& out) noexcept;

/// The request line and headers for a GET. No body, no keep-alive.
///
/// `Connection: close` rather than keep-alive: the device makes one request
/// every few minutes, and a connection held open costs a socket and a
/// timeout-handling path for a saving that would never be measurable.
///
/// One extra header may be added, as a name and a value. The caller checks
/// them with `headerIsSafe` first; this function trusts what it is given.
/// One request header, as a name and a value.
struct Header {
    std::string_view name;
    std::string_view value;
};

/// The most headers a request may carry beyond Host/User-Agent/framing.
/// LibreLinkUp needs five; a bound is the point.
inline constexpr std::size_t kMaxRequestHeaders = 6;

/// The request line, headers and body for a GET or a POST.
///
/// `headers` may include `User-Agent`, which then replaces the default one -
/// some services answer only to the agent string of their own app. A body is
/// sent with its Content-Length; a GET sends none. The caller checks every
/// header with `headerIsSafe` and the method with `methodIsSafe` first; this
/// function trusts what it is given.
std::string buildRequest(const Url& url, std::string_view userAgent, std::string_view method,
                         const Header* headers, std::size_t headerCount, std::string_view body);

/// GET or POST, and nothing else. The device asks for data and logs in to
/// get it; a PUT or a DELETE from here would be a bug, not a feature.
bool methodIsSafe(std::string_view method) noexcept;

std::string buildGet(const Url& url, std::string_view userAgent,
                     std::string_view headerName = {}, std::string_view headerValue = {});

/// Whether a header can go on the wire without splitting the request.
///
/// The name must be a token (RFC 7230: letters, digits and a few marks - no
/// spaces, no colon); the value may carry anything printable but no control
/// characters, which rules out the CR and LF that would start a second header
/// or a second request. An empty name means "no header" and is safe.
bool headerIsSafe(std::string_view name, std::string_view value) noexcept;

/// What came back.
struct Response {
    int status = 0;        ///< 200, 404, ... Zero when nothing was understood.
    std::string body;
    bool complete = false;
    bool chunked = false;

    /// True when the body was cut off at the cap rather than ending.
    ///
    /// Kept rather than hidden, because a script parsing JSON out of a
    /// truncated document will fail in a way that looks like the server
    /// having changed its format.
    bool truncated = false;

    /// The `name=value` of every Set-Cookie, joined with "; " - ready to send
    /// back as a Cookie header. Attributes (Path, Expires...) are dropped.
    /// Capped at kMaxCookieBytes; a session cookie is a few dozen bytes.
    std::string cookies;
};

inline constexpr std::size_t kMaxCookieBytes = 1024;

/// Feeds bytes in, gets a response out.
///
/// Incremental because the bytes arrive in whatever sizes the network feels
/// like, and bounded because the other end of this is a server a script
/// named — which may be hostile, or merely enormous.
class ResponseParser {
public:
    /// Longest body kept, unless the request says otherwise.
    ///
    /// Was one kilobyte, on the reasoning that anything a 52-pixel panel can
    /// show is near the front of the document. That is true of a temperature
    /// and false of a *series*: a contribution heatmap is 365 numbers, the
    /// API that serves them sends 15 KB, and the newest days are at the end -
    /// so a small cap kept precisely the wrong part.
    ///
    /// 24 KB covers that with room to spare. The cost is bounded and was
    /// worked out rather than guessed: ScriptFetcher holds one body per feed
    /// and allows 32 feeds, so the worst case is 768 KB against the 14.3 MB
    /// this device was measured to have free. Still a cap, which is what §38
    /// actually asks for - the device's memory must not depend on what
    /// somebody else's server decided to send.
    /// A source that genuinely needs a whole document asks for a larger cap
    /// per request and pays for it knowingly.
    static constexpr std::size_t kMaxBodyBytes = 24u * 1024u;

    /// Longest header block accepted, so a server that never stops sending
    /// headers cannot hold a buffer open for ever.
    static constexpr std::size_t kMaxHeaderBytes = 4096;

    explicit ResponseParser(std::size_t maxBodyBytes = kMaxBodyBytes) noexcept
        : maxBodyBytes_(maxBodyBytes) {}

    std::size_t maxBodyBytes() const noexcept { return maxBodyBytes_; }

    /// Feed bytes. False means the response is malformed and the connection
    /// should be dropped — not that it is finished.
    bool feed(std::string_view bytes);

    /// True once the whole body has arrived, or once enough of it has that
    /// nothing more will be kept.
    bool done() const noexcept;

    const Response& response() const noexcept { return response_; }

    /// Why it failed, for the log and the panel. Empty when it has not.
    std::string_view failure() const noexcept { return failure_; }

    void reset() noexcept;

private:
    enum class Stage : std::uint8_t { Status, Headers, Body, Chunk, Complete, Broken };

    bool consumeStatusLine(std::string_view line);
    bool consumeHeader(std::string_view line);
    bool consumeBody();
    bool consumeChunks();
    bool fail(const char* why);

    std::size_t maxBodyBytes_;
    Stage stage_ = Stage::Status;
    std::string pending_;
    std::string failure_;
    Response response_;
    std::size_t headerBytes_ = 0;
    /// -1 until a Content-Length is seen.
    long long contentLength_ = -1;
    std::size_t bodyTaken_ = 0;
};

}  // namespace http
}  // namespace net
}  // namespace stipple
