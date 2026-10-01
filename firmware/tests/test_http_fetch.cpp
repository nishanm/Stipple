// SPDX-License-Identifier: GPL-3.0-or-later
//
// Outbound HTTP: the parsing, the schedule and the builtins.
//
// Three layers, three reasons to be careful.
//
// The URL parser is a security boundary. The string comes from a script that
// arrived over the network, and it ends up in a request line - so a control
// character in it is a second request smuggled onto the connection.
//
// The response parser reads bytes from a server nobody here chose. It must
// bound everything and must never loop waiting for something that will not
// come.
//
// The scheduler decides how often somebody else's API gets asked. Getting
// that wrong is not a crash; it is a device quietly hammering a stranger's
// server from inside somebody's house.
#include "stipple/net/HttpFetch.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/net/ScriptFetcher.h"
#include "stipple/platform/simulator/SimulatorHttpClient.h"
#include "stipple/script/ScriptStore.h"
#include "support/TestFramework.h"

using stipple::Canvas;
using stipple::Framebuffer;
using stipple::net::ScriptFetcher;
using stipple::net::http::parseUrl;
using stipple::net::http::ResponseParser;
using stipple::net::http::Url;
using stipple::platform::simulator::SimulatorHttpClient;
using stipple::script::IScriptHttp;
using stipple::script::ScriptPutResult;
using stipple::script::ScriptStore;
namespace colors = stipple::colors;

namespace {

int countLit(const Framebuffer& framebuffer) {
    int lit = 0;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (framebuffer.at(x, y) != colors::kBlack) {
                ++lit;
            }
        }
    }
    return lit;
}

/// Feed a whole response in one go and hand back the parser.
ResponseParser parse(const std::string& bytes) {
    ResponseParser parser;
    parser.feed(bytes);
    return parser;
}

}  // namespace

// --- the URL parser ----------------------------------------------------------

STIPPLE_TEST(HttpUrl, AnOrdinaryUrlComesApart) {
    Url url;
    STIPPLE_REQUIRE(parseUrl("http://example.com/api/v1/now?x=1", url));
    STIPPLE_CHECK(!url.secure);
    STIPPLE_CHECK(url.host == "example.com");
    STIPPLE_CHECK_EQ(url.port, 80);
    STIPPLE_CHECK(url.target == "/api/v1/now?x=1");
}

STIPPLE_TEST(HttpUrl, HttpsIsParsedEvenThoughNothingCanFetchItYet) {
    // Parsed here, refused by the adapter, and the difference matters: this
    // is what lets the refusal say "https not supported yet" instead of
    // "bad url".
    Url url;
    STIPPLE_REQUIRE(parseUrl("https://api.example.com/v3/weather", url));
    STIPPLE_CHECK(url.secure);
    STIPPLE_CHECK_EQ(url.port, 443);
}

STIPPLE_TEST(HttpUrl, APortIsTakenOffTheHost) {
    Url url;
    STIPPLE_REQUIRE(parseUrl("http://192.168.1.10:8123/api/states", url));
    STIPPLE_CHECK(url.host == "192.168.1.10");
    STIPPLE_CHECK_EQ(url.port, 8123);
    STIPPLE_CHECK(url.target == "/api/states");
}

STIPPLE_TEST(HttpUrl, AMissingPathBecomesASlash) {
    Url url;
    STIPPLE_REQUIRE(parseUrl("http://example.com", url));
    STIPPLE_CHECK(url.target == "/");

    STIPPLE_REQUIRE(parseUrl("http://example.com?q=1", url));
    STIPPLE_CHECK(url.target == "/?q=1");
}

STIPPLE_TEST(HttpUrl, AFragmentIsDroppedRatherThanSent) {
    // It never reaches a server, so sending it would be noise on the wire and
    // a needless difference between two URLs that mean the same thing.
    Url url;
    STIPPLE_REQUIRE(parseUrl("http://example.com/page#section", url));
    STIPPLE_CHECK(url.target == "/page");
}

STIPPLE_TEST(HttpUrl, ControlCharactersCannotReachTheRequestLine) {
    // The one check here that is a security boundary rather than a
    // convenience: a newline in the target splits the request and lets a
    // script smuggle a second one onto the connection.
    Url url;
    STIPPLE_CHECK(!parseUrl("http://example.com/a\r\nX-Evil: 1", url));
    STIPPLE_CHECK(!parseUrl(std::string_view("http://example.com/a\nb"), url));
}

STIPPLE_TEST(HttpUrl, CredentialsAreRefusedRatherThanQuietlyDropped) {
    // A URL carrying a password ends up in a script somebody pastes onto a
    // public page. Dropping the credentials silently would produce a 401
    // nobody could explain.
    Url url;
    STIPPLE_CHECK(!parseUrl("http://user:secret@example.com/", url));
}

STIPPLE_TEST(HttpUrl, ASchemeIsRequired) {
    // No guessing. "example.com/x" could be a host or a path, and a parser
    // that decides is a parser that can be surprised.
    Url url;
    STIPPLE_CHECK(!parseUrl("example.com/x", url));
    STIPPLE_CHECK(!parseUrl("//example.com/x", url));
    STIPPLE_CHECK(!parseUrl("ftp://example.com/x", url));
    STIPPLE_CHECK(!parseUrl("file:///etc/passwd", url));
}

STIPPLE_TEST(HttpUrl, NonsenseHostsAreRefused) {
    Url url;
    STIPPLE_CHECK(!parseUrl("http:///just-a-path", url));
    STIPPLE_CHECK(!parseUrl("http://exa mple.com/", url));
    STIPPLE_CHECK(!parseUrl("http://[::1]/", url));   // no IPv6 below this
    STIPPLE_CHECK(!parseUrl("http://example.com:0/", url));
    STIPPLE_CHECK(!parseUrl("http://example.com:99999/", url));
}

STIPPLE_TEST(HttpUrl, AnAbsurdlyLongUrlIsRefused) {
    Url url;
    const std::string huge = "http://example.com/" + std::string(1024, 'a');
    STIPPLE_CHECK(!parseUrl(huge, url));
}

STIPPLE_TEST(HttpUrl, TheRequestOnlyNamesThePortWhenItIsNotTheDefault) {
    // "Host: example.com:80" is legal and confuses enough virtual-host setups
    // to be worth not sending.
    Url plain;
    STIPPLE_REQUIRE(parseUrl("http://example.com/x", plain));
    const std::string a = stipple::net::http::buildGet(plain, "Stipple/test");
    STIPPLE_CHECK(a.find("Host: example.com\r\n") != std::string::npos);

    Url odd;
    STIPPLE_REQUIRE(parseUrl("http://example.com:8080/x", odd));
    const std::string b = stipple::net::http::buildGet(odd, "Stipple/test");
    STIPPLE_CHECK(b.find("Host: example.com:8080\r\n") != std::string::npos);

    // And it closes the connection rather than holding one open for a device
    // that fetches once every five minutes.
    STIPPLE_CHECK(a.find("Connection: close") != std::string::npos);
}

STIPPLE_TEST(HttpUrl, AnExtraHeaderGoesOnTheWireOnce) {
    Url url;
    STIPPLE_REQUIRE(parseUrl("http://example.com/x", url));
    const std::string request =
        stipple::net::http::buildGet(url, "Stipple/test", "api-secret", "abc123");
    const std::size_t at = request.find("\r\napi-secret: abc123\r\n");
    STIPPLE_CHECK(at != std::string::npos);
    // Before the blank line that ends the headers, not after it.
    STIPPLE_CHECK(at < request.find("\r\n\r\n"));

    // No name, no header - and not a dangling colon either.
    const std::string plain = stipple::net::http::buildGet(url, "Stipple/test");
    STIPPLE_CHECK(plain.find("api-secret") == std::string::npos);
    STIPPLE_CHECK(plain.find(": \r\n") == std::string::npos);
}

STIPPLE_TEST(HttpUrl, AHeaderThatCouldSplitTheRequestIsRefused) {
    using stipple::net::http::headerIsSafe;
    STIPPLE_CHECK(headerIsSafe("api-secret", "0123abcd"));
    STIPPLE_CHECK(headerIsSafe("", ""));
    STIPPLE_CHECK_FALSE(headerIsSafe("api-secret", "abc\r\nHost: evil"));
    STIPPLE_CHECK_FALSE(headerIsSafe("api-secret", std::string("a\0b", 3)));
    STIPPLE_CHECK_FALSE(headerIsSafe("api secret", "abc"));
    STIPPLE_CHECK_FALSE(headerIsSafe("api-secret:", "abc"));
    STIPPLE_CHECK_FALSE(headerIsSafe("api\nsecret", "abc"));
}

// --- the response parser -----------------------------------------------------

STIPPLE_TEST(HttpResponse, AnOrdinaryReplyIsRead) {
    const ResponseParser parser = parse(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\n"
        "\r\n{\"power\":1420}");
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK_EQ(parser.response().status, 200);
    STIPPLE_CHECK(parser.response().body == "{\"power\":1420}");
}

STIPPLE_TEST(HttpResponse, BytesMayArriveInAnyPieces) {
    // They will. This is the failure a parser written against one big string
    // has and never sees in testing.
    const std::string whole =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    for (std::size_t split = 1; split < whole.size(); ++split) {
        ResponseParser parser;
        STIPPLE_REQUIRE(parser.feed(whole.substr(0, split)));
        STIPPLE_REQUIRE(parser.feed(whole.substr(split)));
        STIPPLE_CHECK(parser.done());
        STIPPLE_CHECK_EQ(parser.response().status, 200);
        STIPPLE_CHECK(parser.response().body == "hello");
    }
}

STIPPLE_TEST(HttpResponse, AnErrorStatusIsStillAReadableResponse) {
    const ResponseParser parser =
        parse("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot there");
    STIPPLE_CHECK_EQ(parser.response().status, 404);
    STIPPLE_CHECK(parser.done());
}

STIPPLE_TEST(HttpResponse, HeaderNamesAreCaseInsensitive) {
    const ResponseParser parser =
        parse("HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 2\r\n\r\nhi");
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK(parser.response().body == "hi");
}

STIPPLE_TEST(HttpResponse, ChunkedIsUnwrapped) {
    const ResponseParser parser = parse(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK(parser.response().chunked);
    STIPPLE_CHECK(parser.response().body == "hello world");
}

STIPPLE_TEST(HttpResponse, AChunkExtensionIsIgnored) {
    const ResponseParser parser = parse(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;name=value\r\nhello\r\n0\r\n\r\n");
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK(parser.response().body == "hello");
}

STIPPLE_TEST(HttpResponse, A204HasNoBodyWhateverItSays) {
    // Waiting for one would hang until the server closed, which on a keepalive
    // connection is never.
    const ResponseParser parser = parse("HTTP/1.1 204 No Content\r\n\r\n");
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK_EQ(parser.response().status, 204);
    STIPPLE_CHECK(parser.response().body.empty());
}

STIPPLE_TEST(HttpResponse, AHugeBodyIsCutOffAndSaysSo) {
    // The device draws on a panel 52 pixels wide. What matters is that the cap
    // exists and that the truncation is visible - a script parsing JSON out of
    // a cut-off document fails in a way that looks like the server changing
    // its format.
    std::string reply = "HTTP/1.1 200 OK\r\n\r\n";
    reply += std::string(ResponseParser::kMaxBodyBytes * 4, 'x');

    ResponseParser parser;
    STIPPLE_REQUIRE(parser.feed(reply));
    STIPPLE_CHECK_EQ(static_cast<int>(parser.response().body.size()),
                     static_cast<int>(ResponseParser::kMaxBodyBytes));
    STIPPLE_CHECK(parser.response().truncated);
}

STIPPLE_TEST(HttpResponse, ARequestMayAskForALargerBody) {
    // The kilobyte is the default, not the law. A source that needs a whole
    // document says so, and gets exactly what it asked for - no more.
    std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 6000\r\n\r\n";
    reply += std::string(6000, 'y');

    ResponseParser parser(4096);
    STIPPLE_REQUIRE(parser.feed(reply));
    STIPPLE_CHECK_EQ(static_cast<int>(parser.response().body.size()), 4096);
    STIPPLE_CHECK(parser.response().truncated);

    ResponseParser roomy(8192);
    STIPPLE_REQUIRE(roomy.feed(reply));
    STIPPLE_CHECK_EQ(static_cast<int>(roomy.response().body.size()), 6000);
    STIPPLE_CHECK_FALSE(roomy.response().truncated);
    STIPPLE_CHECK(roomy.response().complete);
}

STIPPLE_TEST(HttpResponse, EndlessHeadersAreRefused) {
    // A server that never stops sending headers would otherwise hold a buffer
    // open for as long as it cared to.
    ResponseParser parser;
    STIPPLE_REQUIRE(parser.feed("HTTP/1.1 200 OK\r\n"));
    bool refused = false;
    for (int i = 0; i < 500 && !refused; ++i) {
        refused = !parser.feed("X-Padding: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    }
    STIPPLE_CHECK(refused);
    STIPPLE_CHECK(!parser.failure().empty());
}

STIPPLE_TEST(HttpResponse, SomethingThatIsNotHttpIsRejected) {
    ResponseParser parser;
    STIPPLE_CHECK(!parser.feed("<html>hello</html>\r\n\r\n"));
    STIPPLE_CHECK(!parser.failure().empty());
    STIPPLE_CHECK_EQ(parser.response().status, 0);
}

STIPPLE_TEST(HttpResponse, AnAbsurdContentLengthIsRejected) {
    ResponseParser parser;
    STIPPLE_CHECK(!parser.feed(
        "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999\r\n\r\n"));
}

// --- the schedule ------------------------------------------------------------

namespace {

/// A fetcher with a simulated client behind it, driven by a virtual clock.
struct Rig {
    SimulatorHttpClient client;
    ScriptFetcher fetcher;
    std::uint64_t now = 0;

    Rig() {
        fetcher.setClient(&client);
        fetcher.setNetworkUp(true);
    }

    /// Run the loop forward. Thirty-three milliseconds a tick, like the
    /// device - the schedule has to work at the rate it will actually see.
    void run(std::uint64_t millis) {
        const std::uint64_t until = now + millis;
        while (now < until) {
            fetcher.tick(now);
            now += 33;
        }
        fetcher.tick(now);
    }
};

}  // namespace

STIPPLE_TEST(HttpFetcher, AFollowedUrlIsFetchedAndCached) {
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1420";
    rig.client.answer(route);

    STIPPLE_REQUIRE(rig.fetcher.follow("solar", "http://example.com/now", 0));
    STIPPLE_CHECK(rig.fetcher.body("solar", "http://example.com/now") == nullptr);

    rig.run(1000);

    const std::string* body = rig.fetcher.body("solar", "http://example.com/now");
    STIPPLE_REQUIRE(body != nullptr);
    STIPPLE_CHECK(*body == "1420");
    STIPPLE_CHECK_EQ(rig.fetcher.status("solar", "http://example.com/now"), 200);
    STIPPLE_CHECK(rig.fetcher.ageMillis("solar", "http://example.com/now") >= 0);
}

STIPPLE_TEST(HttpFetcher, AnIntervalIsRespected) {
    // The whole reason the floor exists. Somebody's free API does not want a
    // request a frame from every one of these devices, and the script author
    // is not the person who would find out.
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1";
    rig.client.answer(route);

    STIPPLE_REQUIRE(rig.fetcher.follow("solar", "http://example.com/now", 60000));
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    // Fifty-nine seconds later: still one.
    rig.run(59000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    rig.run(2000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 2);
}

STIPPLE_TEST(HttpFetcher, AScriptCannotAskForAFasterIntervalThanTheFloor) {
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1";
    rig.client.answer(route);

    // One second, asked for. Thirty, given.
    STIPPLE_REQUIRE(rig.fetcher.follow("greedy", "http://example.com/now", 1000));
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    rig.run(20000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    rig.run(12000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 2);
}

STIPPLE_TEST(HttpFetcher, ShorteningTheIntervalDoesNotPullTheNextFetchForward) {
    // A script calling follow() with a smaller number every frame would
    // otherwise fetch continuously while appearing to respect the floor.
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1";
    rig.client.answer(route);

    STIPPLE_REQUIRE(rig.fetcher.follow("sneaky", "http://example.com/now", 600000));
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    for (int i = 0; i < 100; ++i) {
        rig.fetcher.follow("sneaky", "http://example.com/now", 30000);
        rig.run(100);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
}

STIPPLE_TEST(HttpFetcher, OnlyOneRequestRunsAtATime) {
    Rig rig;
    for (int i = 0; i < 4; ++i) {
        SimulatorHttpClient::Route route;
        route.url = "http://example.com/" + std::to_string(i);
        route.body = std::to_string(i);
        route.latencyMillis = 500;
        rig.client.answer(route);
        STIPPLE_REQUIRE(rig.fetcher.follow("s" + std::to_string(i), route.url, 600000));
    }

    // Half a second in: the first has landed, the rest have not started.
    rig.run(600);
    STIPPLE_CHECK(static_cast<int>(rig.client.requests()) <= 2);

    rig.run(3000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 4);
    for (int i = 0; i < 4; ++i) {
        const std::string url = "http://example.com/" + std::to_string(i);
        STIPPLE_CHECK(rig.fetcher.body("s" + std::to_string(i), url) != nullptr);
    }
}

STIPPLE_TEST(HttpFetcher, TheFetcherWaitsWhileSomebodyElseIsUsingTheClient) {
    // The device has one HTTP client and more than one thing that fetches.
    // A request the fetcher did not start is not one it may fail or reset.
    Rig rig;
    SimulatorHttpClient::Route theirs;
    theirs.url = "http://example.com/glucose";
    theirs.body = "[]";
    theirs.latencyMillis = 500;
    rig.client.answer(theirs);
    SimulatorHttpClient::Route ours;
    ours.url = "http://example.com/now";
    ours.body = "1";
    rig.client.answer(ours);

    stipple::platform::HttpRequest request;
    request.url = "http://example.com/glucose";
    STIPPLE_REQUIRE(rig.client.begin(request));
    STIPPLE_REQUIRE(rig.fetcher.follow("solar", "http://example.com/now", 0));

    // Their request completes (they poll it, as its owner); the fetcher
    // neither started its own nor touched theirs - the answer is still there
    // to be collected.
    for (const std::uint64_t until = rig.now + 700; rig.now < until; rig.now += 33) {
        rig.client.poll(rig.now);
        rig.fetcher.tick(rig.now);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
    STIPPLE_CHECK(rig.client.stage() == SimulatorHttpClient::Stage::Done);
    STIPPLE_CHECK(rig.client.body() == "[]");
    STIPPLE_CHECK(rig.fetcher.body("solar", "http://example.com/now") == nullptr);

    // They collect and release it; the fetcher takes its turn.
    rig.client.reset();
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 2);
    STIPPLE_CHECK(rig.fetcher.body("solar", "http://example.com/now") != nullptr);
}

STIPPLE_TEST(HttpFetcher, ForgettingAScriptLeavesSomebodyElsesRequestAlone) {
    Rig rig;
    SimulatorHttpClient::Route theirs;
    theirs.url = "http://example.com/glucose";
    theirs.body = "[]";
    theirs.latencyMillis = 500;
    rig.client.answer(theirs);

    STIPPLE_REQUIRE(rig.fetcher.follow("weather", "http://example.com/w", 600000));
    rig.run(1000);  // the weather feed is fetched (refused by default) and idle again

    stipple::platform::HttpRequest request;
    request.url = "http://example.com/glucose";
    STIPPLE_REQUIRE(rig.client.begin(request));
    rig.fetcher.forget("weather");
    STIPPLE_CHECK(rig.client.stage() == SimulatorHttpClient::Stage::Running);
}

STIPPLE_TEST(SimulatorHttp, RecordsTheHeaderAndCutsTheBodyLikeTheDevice) {
    SimulatorHttpClient client;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/long";
    route.body = "0123456789";
    route.latencyMillis = 0;
    client.answer(route);

    stipple::platform::HttpRequest request;
    request.url = "http://example.com/long";
    request.headerName = "api-secret";
    request.headerValue = "abc";
    request.maxBodyBytes = 4;
    STIPPLE_REQUIRE(client.begin(request));
    client.poll(1);  // schedules the answer
    client.poll(2);  // delivers it
    STIPPLE_REQUIRE(client.stage() == SimulatorHttpClient::Stage::Done);
    STIPPLE_CHECK(client.body() == "0123");
    STIPPLE_CHECK(client.lastTruncated());
    STIPPLE_REQUIRE(!client.askedHeaders().empty());
    STIPPLE_CHECK(client.askedHeaders().back() == "api-secret: abc");

    client.reset();
    stipple::platform::HttpRequest bad;
    bad.url = "http://example.com/long";
    bad.headerName = "api-secret";
    bad.headerValue = "abc\r\nHost: evil";
    STIPPLE_CHECK_FALSE(client.begin(bad));
    STIPPLE_CHECK(client.failure() == "bad header");
}

STIPPLE_TEST(HttpFetcher, AFeedThatNeverAnswersDoesNotStopTheOthers) {
    // One wedged request would otherwise hold the single slot for ever, and
    // the symptom is "the weather app stopped working" on a device where
    // nothing looks wrong.
    Rig rig;
    SimulatorHttpClient::Route stuck;
    stuck.url = "http://example.com/blackhole";
    stuck.hang = true;
    rig.client.answer(stuck);

    SimulatorHttpClient::Route fine;
    fine.url = "http://example.com/fine";
    fine.body = "ok";
    rig.client.answer(fine);

    STIPPLE_REQUIRE(rig.fetcher.follow("a", stuck.url, 600000));
    STIPPLE_REQUIRE(rig.fetcher.follow("b", fine.url, 600000));

    rig.run(ScriptFetcher::kFetchTimeoutMillis + 2000);

    const std::string* body = rig.fetcher.body("b", fine.url);
    STIPPLE_REQUIRE(body != nullptr);
    STIPPLE_CHECK(*body == "ok");
    STIPPLE_CHECK(rig.fetcher.failure("a", stuck.url) == "timed out");
}

STIPPLE_TEST(HttpFetcher, AFailureBacksOffFurtherThanTheInterval) {
    // An endpoint refusing connections is not one to ask every thirty
    // seconds. A device doing that to somebody's server is a device that gets
    // blocked.
    Rig rig;
    SimulatorHttpClient::Route broken;
    broken.url = "http://example.com/down";
    broken.failure = "connection refused";
    rig.client.answer(broken);

    STIPPLE_REQUIRE(rig.fetcher.follow("a", broken.url, 30000));
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
    STIPPLE_CHECK(rig.fetcher.failure("a", broken.url) == "connection refused");

    // Past the interval, inside the backoff.
    rig.run(60000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    rig.run(ScriptFetcher::kFailureBackoffMillis);
    STIPPLE_CHECK(static_cast<int>(rig.client.requests()) >= 2);
}

STIPPLE_TEST(HttpFetcher, AnErrorStatusDoesNotBecomeTheBody) {
    // A 500 with an error page in it is not data, and a script drawing it
    // unexamined would put somebody's stack trace on the panel.
    Rig rig;
    SimulatorHttpClient::Route good;
    good.url = "http://example.com/now";
    good.body = "1420";
    rig.client.answer(good);

    STIPPLE_REQUIRE(rig.fetcher.follow("a", good.url, 30000));
    rig.run(1000);
    const std::string* first = rig.fetcher.body("a", good.url);
    STIPPLE_REQUIRE(first != nullptr);
    STIPPLE_CHECK(*first == "1420");

    SimulatorHttpClient::Route broken;
    broken.url = good.url;
    broken.status = 500;
    broken.body = "<html>Traceback (most recent call last)</html>";
    rig.client.answer(broken);

    rig.run(40000);
    STIPPLE_CHECK_EQ(rig.fetcher.status("a", good.url), 500);
    // The last good reading is still there, and its age says how old.
    const std::string* held = rig.fetcher.body("a", good.url);
    STIPPLE_REQUIRE(held != nullptr);
    STIPPLE_CHECK(*held == "1420");
}

STIPPLE_TEST(HttpFetcher, NothingIsFetchedWhileTheNetworkIsDown) {
    Rig rig;
    rig.fetcher.setNetworkUp(false);
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1";
    rig.client.answer(route);

    STIPPLE_REQUIRE(rig.fetcher.follow("a", route.url, 30000));
    rig.run(10000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 0);
    STIPPLE_CHECK(!rig.fetcher.available());

    rig.fetcher.setNetworkUp(true);
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
}

STIPPLE_TEST(HttpFetcher, AScriptIsToldWhenItsFeedListIsFull) {
    Rig rig;
    for (int i = 0; i < IScriptHttp::kMaxFeedsPerScript; ++i) {
        const std::string url = "http://example.com/" + std::to_string(i);
        STIPPLE_REQUIRE(rig.fetcher.follow("a", url, 30000));
    }
    STIPPLE_CHECK(!rig.fetcher.follow("a", "http://example.com/one-too-many", 30000));
    STIPPLE_CHECK(rig.fetcher.follow("b", "http://example.com/other", 30000));
}

STIPPLE_TEST(HttpFetcher, AUrlThisDeviceWillNotFetchIsRefusedImmediately) {
    // On the frame the script asked, not five minutes later in a log.
    Rig rig;
    STIPPLE_CHECK(!rig.fetcher.follow("a", "file:///etc/passwd", 30000));
    STIPPLE_CHECK(!rig.fetcher.follow("a", "not a url", 30000));
    STIPPLE_CHECK_EQ(rig.fetcher.feedCount(), 0);
}

STIPPLE_TEST(HttpFetcher, ForgettingAScriptStopsFetchingForIt) {
    // A deleted script whose URL was still fetched every five minutes would
    // be a device making requests on behalf of code that no longer exists.
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1";
    rig.client.answer(route);

    STIPPLE_REQUIRE(rig.fetcher.follow("a", route.url, 30000));
    rig.run(1000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);

    rig.fetcher.forget("a");
    STIPPLE_CHECK_EQ(rig.fetcher.feedCount(), 0);

    rig.run(120000);
    STIPPLE_CHECK_EQ(static_cast<int>(rig.client.requests()), 1);
}

// --- the builtins ------------------------------------------------------------

namespace {

bool drawOnce(ScriptStore& store, const char* id, const char* source,
              Framebuffer& framebuffer) {
    if (store.put(id, id, source) != ScriptPutResult::Added) {
        return false;
    }
    Canvas canvas(framebuffer);
    return store.draw(id, canvas, 0);
}

}  // namespace

STIPPLE_TEST(ScriptHttp, ADeviceThatCannotFetchSaysSo) {
    ScriptStore store;  // no fetcher at all
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "off", R"BE(
class App
  def draw()
    if !http_known()
      text(0, 0, 'no net', rgb(255, 0, 0))
    end
    if http_follow('http://example.com/x', 60)
      pixel(51, 15, rgb(0, 255, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));

    STIPPLE_CHECK(countLit(framebuffer) > 0);
    STIPPLE_CHECK(framebuffer.at(51, 15) == colors::kBlack);
}

STIPPLE_TEST(ScriptHttp, AScriptFollowsAUrlAndReadsItBack) {
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/now";
    route.body = "1420";
    rig.client.answer(route);

    ScriptStore store;
    store.setHttp(&rig.fetcher);
    Framebuffer framebuffer;

    const char* source = R"BE(
class App
  def draw()
    http_follow('http://example.com/now', 60)
    var v = http_get('http://example.com/now')
    if v == nil
      text(0, 0, '--', rgb(90, 90, 90))
    else
      text(0, 0, v, rgb(0, 255, 0))
    end
  end
end
return App()
)BE";

    STIPPLE_REQUIRE(drawOnce(store, "solar", source, framebuffer));
    STIPPLE_CHECK(rig.fetcher.feedCount() == 1);

    rig.run(1000);

    Framebuffer after;
    Canvas canvas(after);
    STIPPLE_REQUIRE(store.draw("solar", canvas, 33));
    STIPPLE_CHECK(!(after == framebuffer));
}

STIPPLE_TEST(ScriptHttp, AScriptCanSeeWhyAFetchFailed) {
    // The reason reaches the panel rather than only a log nobody on the
    // device can read.
    Rig rig;
    SimulatorHttpClient::Route broken;
    broken.url = "http://example.com/down";
    broken.failure = "connection refused";
    rig.client.answer(broken);

    ScriptStore store;
    store.setHttp(&rig.fetcher);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "a", R"BE(
class App
  def draw()
    http_follow('http://example.com/down', 60)
    var why = http_error('http://example.com/down')
    if why != nil
      pixel(0, 0, rgb(255, 0, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK(framebuffer.at(0, 0) == colors::kBlack);  // nothing has failed yet

    rig.run(1000);
    Framebuffer after;
    Canvas canvas(after);
    STIPPLE_REQUIRE(store.draw("a", canvas, 33));
    STIPPLE_CHECK(after.at(0, 0) != colors::kBlack);
}

STIPPLE_TEST(ScriptHttp, OneScriptCannotReadAnothersFeed) {
    Rig rig;
    SimulatorHttpClient::Route route;
    route.url = "http://example.com/secret";
    route.body = "hunter2";
    rig.client.answer(route);

    ScriptStore store;
    store.setHttp(&rig.fetcher);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "owner", R"BE(
class App
  def draw()
    http_follow('http://example.com/secret', 60)
  end
end
return App()
)BE",
                             framebuffer));
    rig.run(1000);

    STIPPLE_REQUIRE(drawOnce(store, "nosy", R"BE(
class App
  def draw()
    var v = http_get('http://example.com/secret')
    if v != nil
      pixel(0, 0, rgb(255, 0, 0))
    end
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK(framebuffer.at(0, 0) == colors::kBlack);
}

STIPPLE_TEST(ScriptHttp, DeletingAScriptStopsItsFetching) {
    Rig rig;
    ScriptStore store;
    store.setHttp(&rig.fetcher);
    Framebuffer framebuffer;

    STIPPLE_REQUIRE(drawOnce(store, "a", R"BE(
class App
  def draw()
    http_follow('http://example.com/now', 60)
  end
end
return App()
)BE",
                             framebuffer));
    STIPPLE_CHECK_EQ(rig.fetcher.feedCount(), 1);

    STIPPLE_REQUIRE(store.remove("a"));
    STIPPLE_CHECK_EQ(rig.fetcher.feedCount(), 0);
}

// --- POST, extra headers and cookies, for the glucose services ----------------

STIPPLE_TEST(HttpFetch, BuildRequestWritesAPostWithItsLengthAndHeaders) {
    stipple::net::http::Url url;
    STIPPLE_REQUIRE(stipple::net::http::parseUrl("https://share1.dexcom.com/a/b?x=1", url));
    const stipple::net::http::Header headers[] = {{"Content-Type", "application/json"},
                                                  {"User-Agent", "okhttp/3.5.0"}};
    const std::string wire =
        stipple::net::http::buildRequest(url, "Stipple/1", "POST", headers, 2, "{\"a\":1}");
    STIPPLE_CHECK(wire.rfind("POST /a/b?x=1 HTTP/1.1\r\nHost: share1.dexcom.com\r\n", 0) == 0);
    STIPPLE_CHECK(wire.find("Content-Length: 7\r\n") != std::string::npos);
    // The service's own agent replaces ours rather than joining it.
    STIPPLE_CHECK(wire.find("User-Agent: okhttp/3.5.0") != std::string::npos);
    STIPPLE_CHECK(wire.find("Stipple/1") == std::string::npos);
    STIPPLE_CHECK(wire.size() > 7 && wire.compare(wire.size() - 7, 7, "{\"a\":1}") == 0);
}

STIPPLE_TEST(HttpFetch, SetCookieIsCollectedWithoutItsAttributes) {
    stipple::net::http::ResponseParser parser;
    STIPPLE_REQUIRE(parser.feed("HTTP/1.1 200 OK\r\nSet-Cookie: JSESSIONID=abc; Path=/; HttpOnly\r\n"
                                "Set-Cookie: lang=en\r\nContent-Length: 2\r\n\r\nok"));
    STIPPLE_CHECK(parser.done());
    STIPPLE_CHECK_EQ(parser.response().cookies, std::string("JSESSIONID=abc; lang=en"));
}

STIPPLE_TEST(HttpFetch, RequestChecksRefuseWhatTheDeviceShouldNeverSend) {
    stipple::platform::HttpRequest request;
    request.url = "https://example.com/";
    STIPPLE_CHECK(stipple::platform::requestIsSafe(request));
    request.method = "DELETE";
    STIPPLE_CHECK_FALSE(stipple::platform::requestIsSafe(request));
    request.method = "GET";
    request.body = "x";
    STIPPLE_CHECK_FALSE(stipple::platform::requestIsSafe(request));  // a GET has no body
    request.body = {};
    const stipple::net::http::Header bad[] = {{"X", "a\r\nInjected: 1"}};
    request.headers = bad;
    request.headerCount = 1;
    STIPPLE_CHECK_FALSE(stipple::platform::requestIsSafe(request));
}
