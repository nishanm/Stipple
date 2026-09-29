// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseSource.h"

#include <cstdio>
#include <cstring>

#include "stipple/platform/HttpClient.h"

namespace stipple {
namespace apps {
namespace glucose {
namespace {

constexpr std::uint64_t kMillisPerSecond = 1000;

std::uint64_t periodMillis(int pollSeconds) noexcept {
    return static_cast<std::uint64_t>(pollSeconds) * kMillisPerSecond;
}

/// Insert keeping `out` ascending by epoch; when full, the oldest goes.
int insertSorted(Sample* out, int count, int capacity, const Sample& sample) noexcept {
    if (count == capacity) {
        if (sample.epoch <= out[0].epoch) {
            return count;  // older than everything kept
        }
        for (int i = 1; i < count; ++i) {
            out[i - 1] = out[i];
        }
        --count;
    }
    int at = count;
    while (at > 0 && out[at - 1].epoch > sample.epoch) {
        out[at] = out[at - 1];
        --at;
    }
    out[at] = sample;
    return count + 1;
}

}  // namespace

Trend trendFromDirection(std::string_view direction) noexcept {
    // Lower-cased with the spaces taken out, so "FortyFiveUp", "fortyfiveup"
    // and "Forty Five Up" are one thing; underscores stay, so "NOT_COMPUTABLE"
    // is not mistaken for anything.
    char folded[32];
    std::size_t n = 0;
    for (const char c : direction) {
        if (c == ' ') {
            continue;
        }
        if (n + 1 >= sizeof folded) {
            return Trend::None;
        }
        folded[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - ('A' - 'a')) : c;
    }
    const std::string_view name(folded, n);
    if (name == "doubleup") return Trend::DoubleUp;
    if (name == "singleup") return Trend::SingleUp;
    if (name == "fortyfiveup") return Trend::FortyFiveUp;
    if (name == "flat") return Trend::Flat;
    if (name == "fortyfivedown") return Trend::FortyFiveDown;
    if (name == "singledown") return Trend::SingleDown;
    if (name == "doubledown") return Trend::DoubleDown;
    return Trend::None;
}

Trend trendFromDirectionIndex(std::int64_t index) noexcept {
    switch (index) {
        case 1: return Trend::DoubleUp;
        case 2: return Trend::SingleUp;
        case 3: return Trend::FortyFiveUp;
        case 4: return Trend::Flat;
        case 5: return Trend::FortyFiveDown;
        case 6: return Trend::SingleDown;
        case 7: return Trend::DoubleDown;
        default: return Trend::None;
    }
}

int parseEntries(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                 int tokenCapacity) noexcept {
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return -1;
    }
    const json::Value root = document.root();
    if (!root.isArray()) {
        return -1;
    }

    int count = 0;
    const int entries = root.size();
    for (int i = 0; i < entries; ++i) {
        const json::Value entry = root[i];
        if (!entry.isObject()) {
            continue;
        }
        const std::int64_t sgv = entry["sgv"].toInt(-1);
        if (sgv < 1 || sgv > 1000) {
            continue;
        }
        std::int64_t epoch = entry["date"].toInt(0) / 1000;
        if (epoch <= 0) {
            epoch = entry["mills"].toInt(0) / 1000;
        }
        if (epoch <= 0) {
            continue;
        }
        Sample sample;
        sample.epoch = epoch;
        sample.sgv = static_cast<int>(sgv);
        const json::Value direction = entry["direction"];
        if (direction.isString()) {
            sample.trend = trendFromDirection(direction.raw());
        } else if (direction.isNumber()) {
            sample.trend = trendFromDirectionIndex(direction.toInt(0));
        }
        count = insertSorted(out, count, capacity, sample);
    }
    return count;
}

// --- the poller ---------------------------------------------------------------

std::string NightscoutSource::entriesUrl(std::string_view baseUrl) {
    std::string url(baseUrl);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/api/v1/entries.json?count=";
    url += std::to_string(kEntryCount);
    url += "&find[type]=sgv";
    return url;
}

void NightscoutSource::configure(std::string_view baseUrl, std::string_view apiSecretSha1,
                                 int pollSeconds) {
    const int period = pollSeconds < kMinPollSeconds   ? kMinPollSeconds
                       : pollSeconds > kMaxPollSeconds ? kMaxPollSeconds
                                                       : pollSeconds;
    const std::string url = baseUrl.empty() ? std::string() : entriesUrl(baseUrl);
    if (url == url_ && apiSecretSha1 == headerValue_ && period == pollSeconds_) {
        return;
    }
    url_ = url;
    headerValue_.assign(apiSecretSha1);
    pollSeconds_ = period;
    // A change is a reason to ask straight away, and a new credential is a
    // reason to forget a hold earned by the old one.
    dueMillis_ = 0;
    status_.holdUntilMillis = 0;
    status_.fatalStreak = 0;
    if (url_.empty()) {
        sampleCount_ = 0;
        status_.sampleCount = 0;
        dirty_ = true;
    }
}

void NightscoutSource::tick(std::uint64_t nowMillis, std::int64_t nowUnix, bool wallClockValid,
                            int utcOffsetSeconds) {
    const std::int64_t minute = wallClockValid ? nowUnix / 60 : -1;
    if (dirty_ || minute != lastMinute_ || wallClockValid != lastClockValid_) {
        rebuild(nowUnix, wallClockValid, utcOffsetSeconds);
        lastMinute_ = minute;
        lastClockValid_ = wallClockValid;
    }

    if (client_ == nullptr || url_.empty()) {
        return;
    }
    if (running_) {
        collect(nowMillis);
    }
    if (!running_ && networkUp_ && wallClockValid && nowMillis >= dueMillis_ &&
        nowMillis >= status_.holdUntilMillis) {
        start(nowMillis);
    }
}

void NightscoutSource::start(std::uint64_t nowMillis) {
    // The client is shared. Busy means somebody else's request; wait a tick.
    if (client_->stage() != platform::IHttpClient::Stage::Idle) {
        return;
    }
    platform::HttpRequest request;
    request.url = url_;
    if (!headerValue_.empty()) {
        request.headerName = "api-secret";
        request.headerValue = headerValue_;
    }
    request.maxBodyBytes = kMaxBodyBytes;

    status_.lastAttemptMillis = nowMillis;
    ++status_.fetches;
    if (!client_->begin(request)) {
        char reason[sizeof status_.lastFailure];
        std::snprintf(reason, sizeof reason, "%.*s",
                      static_cast<int>(client_->failure().size()), client_->failure().data());
        client_->reset();
        fail(reason[0] != '\0' ? reason : "cannot fetch", nowMillis, false);
        return;
    }
    running_ = true;
    startedMillis_ = nowMillis;
}

void NightscoutSource::collect(std::uint64_t nowMillis) {
    client_->poll(nowMillis);
    using Stage = platform::IHttpClient::Stage;

    switch (client_->stage()) {
        case Stage::Running:
            if (nowMillis - startedMillis_ < kFetchTimeoutMillis) {
                return;
            }
            fail("timeout", nowMillis, false);
            break;

        case Stage::Done: {
            const int httpStatus = client_->status();
            const std::string_view body = client_->body();
            status_.lastHttpStatus = httpStatus;
            status_.lastBodyBytes = body.size();
            if (httpStatus == 401 || httpStatus == 403) {
                char reason[sizeof status_.lastFailure];
                std::snprintf(reason, sizeof reason, "http %d", httpStatus);
                fail(reason, nowMillis, true);
            } else if (httpStatus < 200 || httpStatus >= 300) {
                char reason[sizeof status_.lastFailure];
                std::snprintf(reason, sizeof reason, "http %d", httpStatus);
                fail(reason, nowMillis, false);
            } else if (body.size() >= kMaxBodyBytes) {
                // The client cut it at the cap; whatever JSON survives is not
                // the document the server sent.
                fail("truncated", nowMillis, false);
            } else {
                const int count = parseEntries(body, incoming_, kMaxHistory, tokens_, kTokenCapacity);
                if (count < 0) {
                    fail("bad data", nowMillis, false);
                } else if (count == 0) {
                    fail("no data", nowMillis, false);
                } else {
                    succeed(nowMillis, count);
                }
            }
            break;
        }

        case Stage::Failed: {
            char reason[sizeof status_.lastFailure];
            std::snprintf(reason, sizeof reason, "%.*s",
                          static_cast<int>(client_->failure().size()), client_->failure().data());
            fail(reason[0] != '\0' ? reason : "fetch failed", nowMillis, false);
            break;
        }

        case Stage::Idle:
            // Somebody reset the client underneath us. Not a failure of the
            // server's; ask again next period.
            dueMillis_ = nowMillis + periodMillis(pollSeconds_);
            break;
    }

    client_->reset();
    running_ = false;
}

void NightscoutSource::succeed(std::uint64_t nowMillis, int count) {
    for (int i = 0; i < count; ++i) {
        samples_[i] = incoming_[i];
    }
    sampleCount_ = count;
    status_.sampleCount = count;
    status_.lastSuccessMillis = nowMillis;
    status_.lastFailure[0] = '\0';
    status_.fatalStreak = 0;
    status_.holdUntilMillis = 0;
    dueMillis_ = nowMillis + periodMillis(pollSeconds_);
    dirty_ = true;
}

void NightscoutSource::fail(const char* reason, std::uint64_t nowMillis, bool fatal) {
    ++status_.failures;
    std::snprintf(status_.lastFailure, sizeof status_.lastFailure, "%s", reason);
    if (fatal) {
        if (status_.fatalStreak < 8) {
            ++status_.fatalStreak;
        }
        const std::uint64_t base =
            periodMillis(pollSeconds_) > kFatalHoldMinMillis ? periodMillis(pollSeconds_)
                                                             : kFatalHoldMinMillis;
        // 5, 10, 20, 30 minutes: the shift is capped where the doubling would
        // pass the ceiling anyway, so a long streak cannot overflow it.
        const unsigned shift = status_.fatalStreak > 3 ? 3u
                                                       : static_cast<unsigned>(status_.fatalStreak - 1);
        const std::uint64_t hold = base << shift;
        status_.holdUntilMillis = nowMillis + (hold > kFatalHoldMaxMillis ? kFatalHoldMaxMillis : hold);
        dueMillis_ = status_.holdUntilMillis;
    } else {
        dueMillis_ = nowMillis + periodMillis(pollSeconds_);
    }
}

void NightscoutSource::rebuild(std::int64_t nowUnix, bool wallClockValid, int utcOffsetSeconds) {
    reading_ = readingFromSamples(samples_, sampleCount_, nowUnix, utcOffsetSeconds, wallClockValid);
    ++revision_;
    dirty_ = false;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
