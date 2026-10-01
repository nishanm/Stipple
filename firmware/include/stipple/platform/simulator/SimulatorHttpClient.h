// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "stipple/platform/HttpClient.h"

namespace stipple {
namespace platform {
namespace simulator {

/// An HTTP client that answers from a table instead of a socket.
///
/// The simulator does not get a real one, and that is the point: a test that
/// reaches the internet is a test that fails when somebody's wifi does, and
/// the emulator runs in a browser where there are no sockets to have. What a
/// script needs to be exercised against is a server that answers, one that
/// 404s, one that hangs and one that refuses — all four of which are easier
/// to arrange here than anywhere real.
///
/// Latency is simulated rather than instant. A fetch that completed inside
/// the same frame it started would hide every ordering bug the real one has.
class SimulatorHttpClient final : public IHttpClient {
public:
    struct Route {
        std::string url;
        int status = 200;
        std::string body;
        /// Milliseconds before the answer appears.
        std::uint32_t latencyMillis = 100;
        /// When set, the fetch fails with this reason instead of answering.
        std::string failure;
        /// When true, it never finishes — for testing the fetcher's timeout.
        bool hang = false;
        /// Sent back as the response's cookies, e.g. "session=abc".
        std::string setCookie;
    };

    /// Add or replace an answer. An exact URL match; no patterns, because a
    /// test that needs two URLs can name two.
    void answer(Route route);

    /// What a URL nobody configured does. Default is a refusal, which is what
    /// an unreachable host looks like and the case scripts most need to
    /// survive.
    void setDefaultFailure(std::string failure) { defaultFailure_ = std::move(failure); }

    /// Answer any URL containing `fragment`, when no exact route matches.
    ///
    /// For harnesses that do not know the URL in advance but do know the
    /// API: the shop previews cannot guess what a card will fetch, but
    /// "anything with socialcounts in it wants a subscriber count" is a rule
    /// they can state up front. Checked in the order registered.
    void answerMatching(std::string fragment, int status, std::string body) {
        Match match;
        match.fragment = std::move(fragment);
        match.status = status;
        match.body = std::move(body);
        matches_.push_back(std::move(match));
    }

    /// Answer every unconfigured URL with this instead of refusing.
    ///
    /// For the shop previews, where the URL a script asks for is not known
    /// until it has drawn a frame and the card would otherwise be three
    /// seconds of the word "fetching".
    void setDefaultAnswer(int status, std::string body) {
        defaultFailure_.clear();
        defaultStatus_ = status;
        defaultBody_ = std::move(body);
    }

    /// Requests begun, for tests that care whether an interval was respected.
    std::uint32_t requests() const noexcept { return requests_; }

    /// The URLs asked for, in order.
    const std::vector<std::string>& asked() const noexcept { return asked_; }

    /// The extra header sent with each request, `name: value`, or empty when
    /// there was none. Parallel to `asked()`.
    const std::vector<std::string>& askedHeaders() const noexcept { return askedHeaders_; }

    /// Every header each request carried, "Name: value" one per line, in the
    /// order given - the legacy single header first.
    const std::vector<std::string>& askedAllHeaders() const noexcept { return askedAllHeaders_; }

    /// The method and body of each request, in order.
    const std::vector<std::string>& askedMethods() const noexcept { return askedMethods_; }
    const std::vector<std::string>& askedBodies() const noexcept { return askedBodies_; }

    /// Whether the last answer was cut at the request's body cap. The device
    /// truncates, so the simulator does too - an emulator that returned a
    /// whole document the panel would never see would be lying.
    bool lastTruncated() const noexcept { return truncated_; }

    // IHttpClient
    using IHttpClient::begin;
    bool begin(const HttpRequest& request) override;
    void poll(std::uint64_t nowMillis) override;
    Stage stage() const noexcept override { return stage_; }
    int status() const noexcept override { return status_; }
    std::string_view body() const noexcept override { return body_; }
    std::string_view failure() const noexcept override { return failure_; }
    std::string_view cookies() const noexcept override { return cookies_; }
    void reset() override;

private:
    const Route* findRoute(std::string_view url) const noexcept;

    struct Match {
        std::string fragment;
        int status = 200;
        std::string body;
    };

    std::vector<Route> routes_;
    std::vector<Match> matches_;
    std::string defaultFailure_ = "cannot reach host";
    int defaultStatus_ = 0;
    std::string defaultBody_;
    std::vector<std::string> asked_;
    std::vector<std::string> askedHeaders_;
    std::vector<std::string> askedAllHeaders_;
    std::vector<std::string> askedMethods_;
    std::vector<std::string> askedBodies_;
    std::string cookies_;
    std::string pendingCookie_;
    std::uint32_t requests_ = 0;
    std::size_t maxBodyBytes_ = net::http::ResponseParser::kMaxBodyBytes;
    bool truncated_ = false;

    Stage stage_ = Stage::Idle;
    std::uint64_t readyAtMillis_ = 0;
    std::uint32_t latencyMillis_ = 0;
    bool hanging_ = false;
    int status_ = 0;
    std::string body_;
    std::string failure_;
    std::string pendingBody_;
    std::string pendingFailure_;
    int pendingStatus_ = 0;
};

}  // namespace simulator
}  // namespace platform
}  // namespace stipple
