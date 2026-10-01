// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/web/StaticFiles.h"

#include <set>
#include <string>

#include "stipple/web/WebAssets.h"
#include "support/TestFramework.h"

using stipple::api::Method;
using stipple::api::Request;
using stipple::api::Response;
using stipple::web::Asset;
using stipple::web::assetCount;
using stipple::web::assets;
using stipple::web::findAsset;
using stipple::web::StaticFiles;

namespace {

Request get(const std::string& path, const std::string& ifNoneMatch = {}) {
    Request request;
    request.method = Method::Get;
    request.path = path;
    request.ifNoneMatch = ifNoneMatch;
    return request;
}

/// Serve a path, asserting that it was ours to serve.
Response serve(const Request& request) {
    StaticFiles files;
    Response response;
    STIPPLE_CHECK(files.tryHandle(request, response));
    return response;
}

}  // namespace

// --- the embedded table ------------------------------------------------------

STIPPLE_TEST(WebAssets, TheUiIsActuallyCompiledIn) {
    // Guards the build wiring rather than the code: if the CMake generator
    // stopped running, every page would 404 on a real device and nothing else
    // in the suite would notice.
    STIPPLE_CHECK(assetCount() >= 3);
    STIPPLE_CHECK(findAsset("/index.html") != nullptr);
    STIPPLE_CHECK(findAsset("/app.css") != nullptr);
    STIPPLE_CHECK(findAsset("/app.js") != nullptr);
}

STIPPLE_TEST(WebAssets, EveryAssetIsWellFormed) {
    for (int i = 0; i < assetCount(); ++i) {
        const Asset& asset = assets()[i];

        STIPPLE_CHECK(!asset.path.empty());
        STIPPLE_CHECK(asset.path.front() == '/');
        STIPPLE_CHECK(!asset.contentType.empty());
        STIPPLE_CHECK(!asset.body.empty());

        // A quoted content hash, as HTTP requires of a strong ETag.
        STIPPLE_CHECK(asset.etag.size() > 2);
        STIPPLE_CHECK(asset.etag.front() == '"');
        STIPPLE_CHECK(asset.etag.back() == '"');
    }
}

STIPPLE_TEST(WebAssets, PathsAreUnique) {
    // Two assets on one path would make lookup depend on table order, and the
    // loser would be unreachable with no error anywhere.
    std::set<std::string_view> seen;
    for (int i = 0; i < assetCount(); ++i) {
        STIPPLE_CHECK(seen.insert(assets()[i].path).second);
    }
}

STIPPLE_TEST(WebAssets, EtagsDifferBetweenDifferentFiles) {
    std::set<std::string_view> seen;
    for (int i = 0; i < assetCount(); ++i) {
        STIPPLE_CHECK(seen.insert(assets()[i].etag).second);
    }
}

STIPPLE_TEST(WebAssets, TheEmbeddedPageSurvivedGeneration) {
    // The generator writes raw string literals. A mangled escape or a delimiter
    // clash would corrupt the content while still compiling, so check for
    // landmarks from each end of the real files.
    const Asset* page = findAsset("/advanced.html");
    const std::string_view html = page->body;
    STIPPLE_CHECK(html.find("<!DOCTYPE html>") != std::string_view::npos);
    STIPPLE_CHECK(html.find("data-setting=\"clock.theme\"") != std::string_view::npos);
    STIPPLE_CHECK(html.find("</html>") != std::string_view::npos);

    const std::string_view script = findAsset("/app.js")->body;
    STIPPLE_CHECK(script.find("STIPPLE_BRIDGE") != std::string_view::npos);
    STIPPLE_CHECK(script.find("/api/v1/settings") != std::string_view::npos);
}

STIPPLE_TEST(WebAssets, TheUiOnlyTalksToTheVersionedApi) {
    // A page that reached for an unversioned path would 404 at runtime against
    // the very rule ADR 0015 established.
    const std::string script(findAsset("/app.js")->body);

    std::size_t at = 0;
    int checked = 0;
    while ((at = script.find("'/api", at)) != std::string::npos) {
        STIPPLE_CHECK(script.compare(at, 9, "'/api/v1/") == 0);
        ++checked;
        at += 5;
    }
    STIPPLE_CHECK(checked > 0);  // the scan found something to check
}

// --- serving -----------------------------------------------------------------

STIPPLE_TEST(StaticFiles, RootServesTheIndex) {
    const Response response = serve(get("/"));

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK_EQ(response.contentType, std::string("text/html; charset=utf-8"));
    STIPPLE_CHECK(response.body.find("<!DOCTYPE html>") != std::string::npos);
}

STIPPLE_TEST(StaticFiles, ServesEachAssetWithItsOwnContentType) {
    STIPPLE_CHECK_EQ(serve(get("/app.css")).contentType,
                    std::string("text/css; charset=utf-8"));
    STIPPLE_CHECK_EQ(serve(get("/app.js")).contentType,
                    std::string("application/javascript; charset=utf-8"));
}

STIPPLE_TEST(StaticFiles, TheControllerPageIsServed) {
    // A phone opens this directly, so it has to be reachable by name rather
    // than only from a link in the single-page app.
    const Response response = serve(get("/gamepad.html"));

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK_EQ(response.contentType, std::string("text/html; charset=utf-8"));

    // Every control it posts must be one /api/v1/input actually accepts.
    // "select" is the name a *script* is given for the knob press; the API
    // calls that switch "press", and sending the wrong one is a 422 that
    // nobody would see until they had a phone and a game in front of them.
    STIPPLE_CHECK(response.body.find("data-control=\"press\"") != std::string::npos);
    STIPPLE_CHECK(response.body.find("data-control=\"select\"") == std::string::npos);

    // The middle button has a place of its own on the page, because it has one
    // on the case. It is also the control ADR 0024 guarantees a script can
    // never take, so a controller that omitted it would be a controller you
    // could not put down.
    STIPPLE_CHECK(response.body.find("data-control=\"middle\"") != std::string::npos);

    // Every data-control on the page must be a name /api/v1/input accepts. The
    // page is laid out to mirror the hardware, and the temptation when doing
    // that is to name a control after where it sits rather than after what the
    // API calls it - which fails as a 422 nobody sees without a phone and a
    // game to hand.
    const std::string marker = "data-control=\"";
    for (std::size_t at = response.body.find(marker); at != std::string::npos;
         at = response.body.find(marker, at + 1)) {
        const std::size_t from = at + marker.size();
        const std::size_t to = response.body.find('"', from);
        STIPPLE_CHECK(to != std::string::npos);
        const std::string name = response.body.substr(from, to - from);
        STIPPLE_CHECK(name == "minus" || name == "plus" || name == "middle" ||
                      name == "press" || name == "left" || name == "right");
    }
}

STIPPLE_TEST(StaticFiles, UnknownPathsAreNotOurs) {
    // Returning false rather than a 404 lets the caller produce one consistent
    // answer instead of two competing ones.
    StaticFiles files;
    Response response;
    STIPPLE_CHECK_FALSE(files.tryHandle(get("/nope.html"), response));
    STIPPLE_CHECK_FALSE(files.tryHandle(get("/api/v1/device"), response));
    STIPPLE_CHECK_FALSE(files.tryHandle(get("/app"), response));
}

STIPPLE_TEST(StaticFiles, AnEmptyPathIsTreatedAsRoot) {
    // Should not arise — a transport always supplies at least "/" — but an
    // empty path landing on the index beats it falling through to an API 404
    // that says nothing useful.
    STIPPLE_CHECK_EQ(serve(get("")).status, 200);
}

STIPPLE_TEST(StaticFiles, TraversalFindsNothingBecauseThereIsNoFilesystem) {
    // Not a defence that has to be maintained: lookup is an exact match against
    // a fixed table, so these paths simply are not in it.
    const char* attempts[] = {
        "/../etc/passwd",      "/./index.html",        "//index.html",
        "/index.html/",        "/index.html%00.txt",   "/..%2f..%2findex.html",
        "/INDEX.HTML",
    };

    StaticFiles files;
    for (const char* path : attempts) {
        Response response;
        STIPPLE_CHECK_FALSE(files.tryHandle(get(path), response));
    }
}

STIPPLE_TEST(StaticFiles, ConditionalRequestGetsA304) {
    const Response first = serve(get("/index.html"));
    STIPPLE_CHECK_EQ(first.status, 200);
    STIPPLE_CHECK(!first.etag.empty());

    const Response second = serve(get("/index.html", first.etag));
    STIPPLE_CHECK_EQ(second.status, 304);
    STIPPLE_CHECK(second.body.empty());
    STIPPLE_CHECK_EQ(second.etag, first.etag);
}

STIPPLE_TEST(StaticFiles, AStaleEtagStillGetsTheBody) {
    const Response response = serve(get("/index.html", "\"something-else\""));

    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(!response.body.empty());
}

STIPPLE_TEST(StaticFiles, HeadAnswersWithoutABody) {
    Request request = get("/index.html");
    request.method = Method::Head;

    const Response response = serve(request);
    STIPPLE_CHECK_EQ(response.status, 200);
    STIPPLE_CHECK(response.body.empty());
    STIPPLE_CHECK(!response.etag.empty());
}

STIPPLE_TEST(StaticFiles, WritingToAPageIsRejectedNotIgnored) {
    const Method writes[] = {Method::Post, Method::Put, Method::Patch, Method::Delete};

    for (Method method : writes) {
        Request request = get("/index.html");
        request.method = method;

        const Response response = serve(request);
        STIPPLE_CHECK_EQ(response.status, 405);
    }
}

STIPPLE_TEST(StaticFiles, PagesRevalidateRatherThanCachingByAge) {
    // A page cached by age would survive a firmware update and show controls
    // that no longer match the API.
    const Response response = serve(get("/index.html"));
    STIPPLE_CHECK(response.cacheControl.find("no-cache") != std::string::npos);
}

STIPPLE_TEST(WebAssets, ThePageDeclaresAnIconSoBrowsersStopGuessing) {
    // A browser not told where the icon is asks for /favicon.ico, which this
    // device does not have - so every visit wrote a 404 into the device's own
    // log. Serving an icon is only half of it; the page has to say so, or the
    // browser never looks.
    const Asset* icon = findAsset("/favicon.svg");
    STIPPLE_REQUIRE(icon != nullptr);
    STIPPLE_CHECK_EQ(std::string(icon->contentType), std::string("image/svg+xml"));

    const Asset* page = findAsset("/index.html");
    STIPPLE_REQUIRE(page != nullptr);
    const std::string html(page->body);
    STIPPLE_CHECK(html.find("rel=\"icon\"") != std::string::npos);
    STIPPLE_CHECK(html.find("/favicon.svg") != std::string::npos);
}

STIPPLE_TEST(WebAssets, TheIconManagerIsWiredToTheElementsItNeeds) {
    // The script looks these up by id and returns quietly if any is missing,
    // which is the right behaviour at runtime and a silent failure here: the
    // section would simply never appear, and nothing would say why.
    const Asset* page = findAsset("/advanced.html");
    STIPPLE_REQUIRE(page != nullptr);
    const std::string html(page->body);

    for (const char* id : {"icon-add", "icon-file", "icon-list", "icon-budget"}) {
        STIPPLE_CHECK(html.find(std::string("id=\"") + id + "\"") != std::string::npos);
    }

    // Multiple files become the frames of one animation, so the picker has to
    // accept more than one.
    STIPPLE_CHECK(html.find("id=\"icon-file\"") != std::string::npos);
    STIPPLE_CHECK(html.find("multiple") != std::string::npos);

    const Asset* script = findAsset("/app.js");
    STIPPLE_REQUIRE(script != nullptr);
    const std::string js(script->body);

    // Conversion happens in the browser on purpose: decoding PNG or GIF on
    // the device would mean running inflate or LZW over a file somebody
    // uploaded.
    STIPPLE_CHECK(js.find("/api/v1/assets") != std::string::npos);
    STIPPLE_CHECK(js.find("wireIcons") != std::string::npos);
    STIPPLE_CHECK(js.find("loadIcons") != std::string::npos);
}

// --- the glucose page at the front door -----------------------------------------

STIPPLE_TEST(WebAssets, TheFrontPageIsTheGlucoseClocksFourTabs) {
    const Asset* page = findAsset("/index.html");
    STIPPLE_REQUIRE(page != nullptr);
    const std::string html(page->body);
    for (const char* tab : {"data-tab=\"display\"", "data-tab=\"glucose\"", "data-tab=\"alarms\"",
                            "data-tab=\"system\""}) {
        STIPPLE_CHECK(html.find(tab) != std::string::npos);
    }
    STIPPLE_CHECK(html.find("/glucose.js") != std::string::npos);
    STIPPLE_CHECK(html.find("/glucose.css") != std::string::npos);
    STIPPLE_CHECK(html.find("rel=\"icon\"") != std::string::npos);
    STIPPLE_CHECK(findAsset("/glucose.js") != nullptr);
    STIPPLE_CHECK(findAsset("/glucose.css") != nullptr);
    // Everything else is still one link away.
    STIPPLE_CHECK(findAsset("/advanced.html") != nullptr);
    STIPPLE_CHECK(std::string(findAsset("/glucose.js")->body).find("/advanced.html") != std::string::npos);
    STIPPLE_CHECK(std::string(findAsset("/advanced.html")->body).find("href=\"/\"") != std::string::npos);
}

STIPPLE_TEST(WebAssets, TheGlucosePageOnlyTalksToTheVersionedApi) {
    const std::string script(findAsset("/glucose.js")->body);
    std::size_t at = 0;
    int checked = 0;
    while ((at = script.find("'/api", at)) != std::string::npos) {
        STIPPLE_CHECK(script.compare(at, 9, "'/api/v1/") == 0);
        ++checked;
        at += 5;
    }
    STIPPLE_CHECK(checked > 5);
    // The emulator's bridge, like the advanced page.
    STIPPLE_CHECK(script.find("STIPPLE_BRIDGE") != std::string::npos);
}
