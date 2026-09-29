// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/net/ScriptFetcher.h"

#include <utility>

#include "stipple/net/HttpFetch.h"

namespace stipple {
namespace net {

using script::IScriptHttp;

bool ScriptFetcher::available() const noexcept {
    return client_ != nullptr && networkUp_;
}

bool ScriptFetcher::follow(std::string_view scriptId, std::string_view url,
                           std::uint32_t intervalMillis) {
    if (scriptId.empty()) {
        return false;
    }

    // Parsed here rather than at fetch time, so a script gets its "no" on the
    // frame it asked rather than five minutes later in a log.
    http::Url parsed;
    if (!http::parseUrl(url, parsed)) {
        return false;
    }

    if (intervalMillis == 0) {
        intervalMillis = IScriptHttp::kDefaultIntervalMillis;
    }
    if (intervalMillis < IScriptHttp::kMinIntervalMillis) {
        intervalMillis = IScriptHttp::kMinIntervalMillis;
    }
    if (intervalMillis > IScriptHttp::kMaxIntervalMillis) {
        intervalMillis = IScriptHttp::kMaxIntervalMillis;
    }

    if (Feed* held = find(scriptId, url); held != nullptr) {
        // Changing the interval does not pull the next fetch forward. A script
        // that shortened its interval every frame would otherwise be a way to
        // fetch continuously while appearing to respect the floor.
        held->intervalMillis = intervalMillis;
        return true;
    }

    int mine = 0;
    for (const Feed& held : feeds_) {
        if (held.scriptId == scriptId) {
            ++mine;
        }
    }
    if (mine >= IScriptHttp::kMaxFeedsPerScript ||
        static_cast<int>(feeds_.size()) >= kMaxFeedsTotal) {
        return false;
    }

    Feed added;
    added.scriptId.assign(scriptId);
    added.url.assign(url);
    added.intervalMillis = intervalMillis;
    added.dueMillis = 0;  // as soon as the schedule reaches it
    feeds_.push_back(std::move(added));
    return true;
}

const std::string* ScriptFetcher::body(std::string_view scriptId,
                                       std::string_view url) const {
    const Feed* held = find(scriptId, url);
    if (held == nullptr || !held->seen) {
        return nullptr;
    }
    return &held->body;
}

int ScriptFetcher::status(std::string_view scriptId, std::string_view url) const {
    const Feed* held = find(scriptId, url);
    return held == nullptr ? 0 : held->status;
}

std::int64_t ScriptFetcher::ageMillis(std::string_view scriptId,
                                      std::string_view url) const {
    const Feed* held = find(scriptId, url);
    if (held == nullptr || !held->seen) {
        return -1;
    }
    if (nowMillis_ < held->arrivedMillis) {
        return 0;
    }
    return static_cast<std::int64_t>(nowMillis_ - held->arrivedMillis);
}

std::string_view ScriptFetcher::failure(std::string_view scriptId,
                                        std::string_view url) const {
    const Feed* held = find(scriptId, url);
    return held == nullptr ? std::string_view() : std::string_view(held->failure);
}

void ScriptFetcher::forget(std::string_view scriptId) {
    // A fetch already in flight for this script is abandoned rather than
    // waited for. Its result has nowhere to go.
    if (running_ >= 0 && static_cast<std::size_t>(running_) < feeds_.size() &&
        feeds_[static_cast<std::size_t>(running_)].scriptId == scriptId) {
        if (client_ != nullptr) {
            client_->reset();
        }
        running_ = -1;
    }

    std::size_t write = 0;
    for (std::size_t read = 0; read < feeds_.size(); ++read) {
        if (feeds_[read].scriptId != scriptId) {
            if (write != read) {
                feeds_[write] = std::move(feeds_[read]);
            }
            ++write;
        }
    }
    if (write != feeds_.size()) {
        // Indices into the vector are about to move, so nothing may be left
        // pointing into it. Only a request of ours is reset: when nothing is
        // running the client may be carrying somebody else's fetch, and a
        // script being deleted is no reason to abort it.
        feeds_.resize(write);
        if (running_ >= 0) {
            running_ = -1;
            if (client_ != nullptr) {
                client_->reset();
            }
        }
    }
}

void ScriptFetcher::tick(std::uint64_t nowMillis) {
    nowMillis_ = nowMillis;
    if (client_ == nullptr) {
        return;
    }

    collect(nowMillis);

    if (running_ < 0 && networkUp_) {
        start(nowMillis);
    }
}

void ScriptFetcher::collect(std::uint64_t nowMillis) {
    if (running_ < 0) {
        return;
    }
    if (static_cast<std::size_t>(running_) >= feeds_.size()) {
        running_ = -1;
        client_->reset();
        return;
    }

    client_->poll(nowMillis);
    Feed& feed = feeds_[static_cast<std::size_t>(running_)];

    switch (client_->stage()) {
        case platform::IHttpClient::Stage::Running:
            if (nowMillis - startedMillis_ < kFetchTimeoutMillis) {
                return;
            }
            // The adapter has lost track. Say so plainly rather than leaving
            // the feed running for ever: one wedged request would otherwise
            // stop every other feed on the device.
            feed.failure = "timed out";
            feed.status = 0;
            feed.dueMillis = nowMillis + kFailureBackoffMillis;
            break;

        case platform::IHttpClient::Stage::Done:
            feed.status = client_->status();
            feed.failure.clear();
            // Only a 2xx becomes the body. A 500 with an error page in it is
            // not data, and a script drawing it unexamined would put
            // somebody's stack trace on the panel - so the status is kept, the
            // old body is kept, and the script can see both.
            if (feed.status >= 200 && feed.status < 300) {
                feed.body.assign(client_->body());
                feed.arrivedMillis = nowMillis;
                feed.seen = true;
            }
            feed.dueMillis = nowMillis + feed.intervalMillis;
            break;

        case platform::IHttpClient::Stage::Failed:
            feed.failure.assign(client_->failure());
            if (feed.failure.empty()) {
                feed.failure = "fetch failed";
            }
            feed.status = 0;
            feed.dueMillis = nowMillis + kFailureBackoffMillis;
            break;

        case platform::IHttpClient::Stage::Idle:
            // Somebody reset the client underneath us. Reschedule normally
            // rather than treating it as a failure.
            feed.dueMillis = nowMillis + feed.intervalMillis;
            break;
    }

    client_->reset();
    running_ = -1;
}

void ScriptFetcher::start(std::uint64_t nowMillis) {
    // The feed that has been due longest, so a script asking for a short
    // interval cannot starve one asking for a long one.
    int best = -1;
    std::uint64_t bestDue = 0;
    for (std::size_t i = 0; i < feeds_.size(); ++i) {
        const Feed& feed = feeds_[i];
        if (feed.dueMillis > nowMillis) {
            continue;
        }
        if (best < 0 || feed.dueMillis < bestDue) {
            best = static_cast<int>(i);
            bestDue = feed.dueMillis;
        }
    }
    if (best < 0) {
        return;
    }

    // The client is shared with anything else on the device that fetches.
    // Busy means somebody else's request is in flight; wait a tick rather
    // than begin() on it, which would fail and then reset() their request.
    if (client_->stage() != platform::IHttpClient::Stage::Idle) {
        return;
    }

    Feed& feed = feeds_[static_cast<std::size_t>(best)];
    if (!client_->begin(feed.url)) {
        // Refused outright - an https URL on a build with no TLS, say. Kept as
        // the feed's failure so it reaches the panel rather than only the log,
        // and backed off so a refusal is not retried every frame.
        feed.failure.assign(client_->failure());
        if (feed.failure.empty()) {
            feed.failure = "cannot fetch";
        }
        feed.dueMillis = nowMillis + kFailureBackoffMillis;
        client_->reset();
        return;
    }

    running_ = best;
    startedMillis_ = nowMillis;
    ++started_;
}

ScriptFetcher::Feed* ScriptFetcher::find(std::string_view scriptId,
                                         std::string_view url) noexcept {
    for (Feed& feed : feeds_) {
        if (feed.scriptId == scriptId && feed.url == url) {
            return &feed;
        }
    }
    return nullptr;
}

const ScriptFetcher::Feed* ScriptFetcher::find(std::string_view scriptId,
                                               std::string_view url) const noexcept {
    for (const Feed& feed : feeds_) {
        if (feed.scriptId == scriptId && feed.url == url) {
            return &feed;
        }
    }
    return nullptr;
}

}  // namespace net
}  // namespace stipple
