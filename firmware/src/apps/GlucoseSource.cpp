// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseSource.h"

#include <cstdio>
#include <cstring>

#include "stipple/core/Sha256.h"
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

std::string GlucoseSource::entriesUrl(std::string_view baseUrl) {
    std::string url(baseUrl);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/api/v1/entries.json?count=";
    url += std::to_string(kEntryCount);
    url += "&find[type]=sgv";
    return url;
}

// --- configuration ------------------------------------------------------------

void GlucoseSource::configure(std::string_view baseUrl, std::string_view apiSecretSha1,
                              int pollSeconds) {
    SourceSettings settings;
    settings.kind = SourceKind::Nightscout;
    settings.url.assign(baseUrl);
    settings.apiSecretSha1.assign(apiSecretSha1);
    configure(settings, pollSeconds);
}

void GlucoseSource::configure(const SourceSettings& settings, int pollSeconds) {
    const int period = pollSeconds < kMinPollSeconds   ? kMinPollSeconds
                       : pollSeconds > kMaxPollSeconds ? kMaxPollSeconds
                                                       : pollSeconds;
    if (settings == settings_ && period == pollSeconds_) {
        return;
    }
    const bool sameService = settings.kind == settings_.kind && settings.url == settings_.url &&
                             settings.username == settings_.username &&
                             settings.region == settings_.region &&
                             settings.patientId == settings_.patientId;
    // A request in flight was asked under the old settings; its answer must
    // not land under the new ones - another site's, or another person's,
    // reading shown as this one's.
    if (running_ && client_ != nullptr) {
        client_->reset();
    }
    running_ = false;
    step_ = Step::None;
    settings_ = settings;
    url_ = settings_.kind == SourceKind::Nightscout && !settings_.url.empty()
               ? entriesUrl(settings_.url)
               : std::string();
    pollSeconds_ = period;
    dueMillis_ = 0;
    status_.holdUntilMillis = 0;
    status_.fatalStreak = 0;
    dropSession();
    libreRegion_ = settings_.region;
    if (!sameService || !configured()) {
        // Another service's (or another person's) readings must not carry on
        // under this one's name.
        sampleCount_ = 0;
        status_.sampleCount = 0;
        status_.patientCount = 0;
        dirty_ = true;
    }
}

bool GlucoseSource::configured() const noexcept {
    switch (settings_.kind) {
        case SourceKind::Nightscout: return !url_.empty();
        case SourceKind::Dexcom:
            return !settings_.username.empty() && !settings_.password.empty() &&
                   dexcomBaseUrl(settings_.region) != nullptr;
        case SourceKind::LibreLinkUp:
            return !settings_.username.empty() && !settings_.password.empty() &&
                   libreHost(settings_.region) != nullptr;
        case SourceKind::Medtrum:
            return !settings_.username.empty() && !settings_.password.empty();
    }
    return false;
}

int GlucoseSource::effectivePollSeconds() const noexcept {
    if (settings_.kind != SourceKind::Nightscout && pollSeconds_ < kMinCloudPollSeconds) {
        return kMinCloudPollSeconds;
    }
    return pollSeconds_;
}

void GlucoseSource::dropSession() noexcept {
    dexcomAccountId_.clear();
    dexcomSessionId_.clear();
    libreToken_.clear();
    libreExpires_ = 0;
    libreAccountHash_.clear();
    librePatientId_.clear();
    medtrumCookie_.clear();
    medtrumUser_.clear();
}

std::int64_t GlucoseSource::newestEpoch() const noexcept {
    return sampleCount_ > 0 ? samples_[sampleCount_ - 1].epoch : 0;
}

// --- driving ------------------------------------------------------------------

void GlucoseSource::tick(std::uint64_t nowMillis, std::int64_t nowUnix, bool wallClockValid,
                         int utcOffsetSeconds) {
    nowUnix_ = wallClockValid ? nowUnix : 0;
    const std::int64_t minute = wallClockValid ? nowUnix / 60 : -1;
    if (dirty_ || minute != lastMinute_ || wallClockValid != lastClockValid_) {
        rebuild(nowUnix, wallClockValid, utcOffsetSeconds);
        lastMinute_ = minute;
        lastClockValid_ = wallClockValid;
    }

    if (client_ == nullptr || !configured()) {
        return;
    }
    if (running_) {
        collect(nowMillis);
    }
    if (!running_ && networkUp_ && wallClockValid && nowMillis >= dueMillis_ &&
        nowMillis >= status_.holdUntilMillis) {
        if (step_ == Step::None) {
            step_ = firstStep();
            stepsThisPoll_ = 0;
            renewedThisPoll_ = false;
        }
        start(nowMillis);
    }
}

GlucoseSource::Step GlucoseSource::firstStep() const noexcept {
    switch (settings_.kind) {
        case SourceKind::Nightscout: return Step::NightscoutEntries;
        case SourceKind::Dexcom:
            return dexcomAccountId_.empty()   ? Step::DexcomAccount
                   : dexcomSessionId_.empty() ? Step::DexcomSession
                                              : Step::DexcomReadings;
        case SourceKind::LibreLinkUp:
            // A minute's grace, so a token is not used in the second it dies.
            return libreToken_.empty() || (libreExpires_ > 0 && nowUnix_ + 60 >= libreExpires_)
                       ? Step::LibreLogin
                       : Step::LibreConnections;
        case SourceKind::Medtrum:
            return medtrumCookie_.empty() ? Step::MedtrumLogin : Step::MedtrumMonitor;
    }
    return Step::None;
}

void GlucoseSource::start(std::uint64_t nowMillis) {
    if (client_->stage() != platform::IHttpClient::Stage::Idle) {
        return;
    }
    if (stepsThisPoll_ >= kMaxStepsPerPoll) {
        step_ = Step::None;
        fail("too many steps", nowMillis, false);
        return;
    }

    platform::HttpRequest request;
    net::http::Header headers[net::http::kMaxRequestHeaders];
    std::size_t count = 0;
    request.maxBodyBytes = kMaxBodyBytes;
    requestBody_.clear();

    const std::string& dexBase =
        settings_.kind == SourceKind::Dexcom && dexcomBaseUrl(settings_.region) != nullptr
            ? std::string(dexcomBaseUrl(settings_.region))
            : std::string();
    const char* libre = libreHost(libreRegion_.empty() ? settings_.region : libreRegion_);
    const std::string libreBase = libre != nullptr ? std::string("https://") + libre : std::string();

    auto libreHeaders = [&](bool authorised) {
        headers[count++] = {"User-Agent", kLibreUserAgent};
        headers[count++] = {"Content-Type", "application/json;charset=UTF-8"};
        headers[count++] = {"version", kLibreVersion};
        headers[count++] = {"product", kLibreProduct};
        if (authorised) {
            headerValues_[0] = "Bearer " + libreToken_;
            headers[count++] = {"Authorization", headerValues_[0]};
            headers[count++] = {"account-id", libreAccountHash_};
        }
    };
    auto medtrumHeaders = [&]() {
        headers[count++] = {"User-Agent", kMedtrumUserAgent};
        headers[count++] = {"DevInfo", kMedtrumDevInfo};
        headers[count++] = {"AppTag", kMedtrumAppTag};
    };

    switch (step_) {
        case Step::NightscoutEntries:
            lastUrl_ = url_;
            if (!settings_.apiSecretSha1.empty()) {
                request.headerName = "api-secret";
                request.headerValue = settings_.apiSecretSha1;
            }
            break;
        case Step::DexcomAccount:
            lastUrl_ = dexBase + std::string(kDexcomAccountPath);
            requestBody_ = dexcomAccountBody(settings_.username, settings_.password, settings_.region);
            break;
        case Step::DexcomSession:
            lastUrl_ = dexBase + std::string(kDexcomSessionPath);
            requestBody_ = dexcomSessionBody(dexcomAccountId_, settings_.password, settings_.region);
            break;
        case Step::DexcomReadings:
            lastUrl_ = dexBase + std::string(kDexcomReadingsPath) + "?sessionId=" + dexcomSessionId_ +
                       "&minutes=" + std::to_string(kDexcomMinutes) +
                       "&maxCount=" + std::to_string(kDexcomMaxCount);
            requestBody_ = "{}";
            break;
        case Step::LibreLogin:
            lastUrl_ = libreBase + "/llu/auth/login";
            requestBody_ = libreLoginBody(settings_.username, settings_.password);
            libreHeaders(false);
            break;
        case Step::LibreConnections:
            lastUrl_ = libreBase + "/llu/connections";
            libreHeaders(true);
            break;
        case Step::LibreGraph:
            lastUrl_ = libreBase + "/llu/connections/" + librePatientId_ + "/graph";
            libreHeaders(true);
            request.maxBodyBytes = kMaxGraphBodyBytes;
            break;
        case Step::MedtrumLogin:
            lastUrl_ = std::string(kMedtrumLoginUrl);
            requestBody_ = medtrumLoginBody(settings_.username, settings_.password);
            medtrumHeaders();
            headers[count++] = {"Content-Type", "application/x-www-form-urlencoded"};
            break;
        case Step::MedtrumMonitor:
            lastUrl_ = std::string(kMedtrumMonitorUrl);
            medtrumHeaders();
            headers[count++] = {"Cookie", medtrumCookie_};
            break;
        case Step::MedtrumHistory: {
            // An hour a request, as nightscout-clock asks: the service limits
            // how much one answer carries.
            const std::int64_t to =
                medtrumFrom_ + 3600 < nowUnix_ ? medtrumFrom_ + 3600 : nowUnix_;
            lastUrl_ = medtrumHistoryUrl(medtrumFrom_, to, medtrumUser_);
            medtrumHeaders();
            headers[count++] = {"Cookie", medtrumCookie_};
            break;
        }
        case Step::None:
            return;
    }

    const bool dexcom = settings_.kind == SourceKind::Dexcom;
    if (dexcom) {
        headers[count++] = {"Content-Type", "application/json"};
        headers[count++] = {"Accept", "application/json"};
    }

    request.url = lastUrl_;
    request.headers = headers;
    request.headerCount = count;
    if (!requestBody_.empty()) {
        request.method = "POST";
        request.body = requestBody_;
    }

    status_.lastAttemptMillis = nowMillis;
    ++status_.fetches;
    ++stepsThisPoll_;
    if (!client_->begin(request)) {
        char reason[sizeof status_.lastFailure];
        std::snprintf(reason, sizeof reason, "%.*s",
                      static_cast<int>(client_->failure().size()), client_->failure().data());
        client_->reset();
        step_ = Step::None;
        fail(reason[0] != '\0' ? reason : "cannot fetch", nowMillis, false);
        return;
    }
    running_ = true;
    startedMillis_ = nowMillis;
}

void GlucoseSource::collect(std::uint64_t nowMillis) {
    client_->poll(nowMillis);
    using Stage = platform::IHttpClient::Stage;

    switch (client_->stage()) {
        case Stage::Running:
            if (nowMillis - startedMillis_ < kFetchTimeoutMillis) {
                return;
            }
            step_ = Step::None;
            fail("timeout", nowMillis, false);
            break;

        case Stage::Done: {
            const int httpStatus = client_->status();
            const std::string_view body = client_->body();
            status_.lastHttpStatus = httpStatus;
            status_.lastBodyBytes = body.size();
            const Outcome outcome = answer(httpStatus, body, client_->cookies());
            switch (outcome.kind) {
                case Outcome::Kind::Next:
                    // Straight on to the next request of this poll.
                    step_ = outcome.next;
                    dueMillis_ = 0;
                    break;
                case Outcome::Kind::Samples:
                    step_ = Step::None;
                    succeed(nowMillis, outcome);
                    break;
                case Outcome::Kind::Fail:
                    step_ = Step::None;
                    fail(outcome.reason, nowMillis, outcome.fatal);
                    break;
            }
            break;
        }

        case Stage::Failed: {
            char reason[sizeof status_.lastFailure];
            std::snprintf(reason, sizeof reason, "%.*s",
                          static_cast<int>(client_->failure().size()), client_->failure().data());
            step_ = Step::None;
            fail(reason[0] != '\0' ? reason : "fetch failed", nowMillis, false);
            break;
        }

        case Stage::Idle:
            // Somebody reset the client under us. Not a failure of the service.
            step_ = Step::None;
            dueMillis_ = nowMillis + periodMillis(effectivePollSeconds());
            break;
    }

    client_->reset();
    running_ = false;
}

// --- answers ------------------------------------------------------------------

namespace {

/// A refusal of the credential or of the rate: hold off, do not retry.
bool refused(int httpStatus) noexcept {
    return httpStatus == 401 || httpStatus == 403 || httpStatus == 429;
}

}  // namespace

GlucoseSource::Outcome GlucoseSource::answer(int httpStatus, std::string_view body,
                                             std::string_view cookies) {
    switch (settings_.kind) {
        case SourceKind::Nightscout: return answerNightscout(httpStatus, body);
        case SourceKind::Dexcom: return answerDexcom(httpStatus, body);
        case SourceKind::LibreLinkUp: return answerLibre(httpStatus, body);
        case SourceKind::Medtrum: return answerMedtrum(httpStatus, body, cookies);
    }
    return Outcome{};
}

GlucoseSource::Outcome GlucoseSource::answerNightscout(int httpStatus, std::string_view body) {
    Outcome out;
    static char reason[24];
    if (httpStatus < 200 || httpStatus >= 300) {
        std::snprintf(reason, sizeof reason, "http %d", httpStatus);
        out.reason = reason;
        out.fatal = httpStatus == 401 || httpStatus == 403;
        return out;
    }
    if (body.size() >= kMaxBodyBytes) {
        out.reason = "truncated";
        return out;
    }
    const int count = parseEntries(body, incoming_, kMaxHistory, tokens_, kTokenCapacity);
    if (count < 0) {
        out.reason = "bad data";
    } else if (count == 0) {
        out.reason = "no data";
    } else {
        out.kind = Outcome::Kind::Samples;
        out.replace = true;
        out.count = count;
    }
    return out;
}

GlucoseSource::Outcome GlucoseSource::answerDexcom(int httpStatus, std::string_view body) {
    Outcome out;
    static char reason[24];
    if (httpStatus != 200) {
        if (refused(httpStatus) || dexcomCredentialsRejected(body)) {
            out.reason = "wrong password";
            out.fatal = true;
            dropSession();
            return out;
        }
        if (step_ == Step::DexcomReadings && dexcomSessionExpired(body) && !renewedThisPoll_) {
            renewedThisPoll_ = true;
            dexcomSessionId_.clear();
            out.kind = Outcome::Kind::Next;
            out.next = Step::DexcomSession;
            return out;
        }
        std::snprintf(reason, sizeof reason, "http %d", httpStatus);
        out.reason = reason;
        return out;
    }
    switch (step_) {
        case Step::DexcomAccount:
            dexcomAccountId_ = dexcomQuotedId(body);
            if (dexcomAccountId_.empty()) {
                out.reason = "no dexcom account";
                out.fatal = true;
                return out;
            }
            out.kind = Outcome::Kind::Next;
            out.next = Step::DexcomSession;
            return out;
        case Step::DexcomSession:
            dexcomSessionId_ = dexcomQuotedId(body);
            if (dexcomSessionId_.empty()) {
                // The all-zeros session: the account exists but this login
                // was not accepted. Start again from the account next time.
                dropSession();
                out.reason = "wrong password";
                out.fatal = true;
                return out;
            }
            out.kind = Outcome::Kind::Next;
            out.next = Step::DexcomReadings;
            return out;
        case Step::DexcomReadings: {
            const int count = parseDexcomReadings(body, incoming_, kMaxHistory, tokens_,
                                                  kTokenCapacity);
            if (count < 0) {
                out.reason = "bad data";
            } else if (count == 0) {
                out.reason = "no data";
            } else {
                out.kind = Outcome::Kind::Samples;
                out.replace = true;
                out.count = count;
            }
            return out;
        }
        default:
            return out;
    }
}

GlucoseSource::Outcome GlucoseSource::answerLibre(int httpStatus, std::string_view body) {
    Outcome out;
    static char reason[24];
    if (httpStatus == 401 && step_ != Step::LibreLogin && !renewedThisPoll_) {
        renewedThisPoll_ = true;
        libreToken_.clear();
        out.kind = Outcome::Kind::Next;
        out.next = Step::LibreLogin;
        return out;
    }
    if (refused(httpStatus)) {
        out.reason = httpStatus == 429 ? "too many requests" : "wrong password";
        out.fatal = true;
        dropSession();
        return out;
    }
    if (httpStatus != 200) {
        std::snprintf(reason, sizeof reason, "http %d", httpStatus);
        out.reason = reason;
        return out;
    }

    switch (step_) {
        case Step::LibreLogin: {
            LibreLogin login;
            if (!parseLibreLogin(body, login, tokens_, kTokenCapacity)) {
                out.reason = "bad data";
                return out;
            }
            if (login.redirect) {
                if (libreHost(login.region) == nullptr || renewedThisPoll_) {
                    out.reason = "unknown region";
                    return out;
                }
                renewedThisPoll_ = true;
                libreRegion_ = login.region;
                std::snprintf(status_.region, sizeof status_.region, "%s", libreRegion_.c_str());
                out.kind = Outcome::Kind::Next;
                out.next = Step::LibreLogin;
                return out;
            }
            if (login.status == 4) {
                out.reason = "accept terms in app";
                out.fatal = true;
                return out;
            }
            if (login.status != 0 || login.token.empty() || login.userId.empty()) {
                out.reason = "wrong password";
                out.fatal = true;
                return out;
            }
            libreToken_ = login.token;
            libreExpires_ = login.expires;
            libreAccountHash_ = Sha256::hex(login.userId.data(), login.userId.size());
            std::snprintf(status_.region, sizeof status_.region, "%s",
                          (libreRegion_.empty() ? settings_.region : libreRegion_).c_str());
            out.kind = Outcome::Kind::Next;
            out.next = Step::LibreConnections;
            return out;
        }
        case Step::LibreConnections: {
            LibreConnections connections;
            if (!parseLibreConnections(body, settings_.patientId, connections, tokens_,
                                       kTokenCapacity) ||
                connections.status != 0) {
                out.reason = "bad data";
                return out;
            }
            status_.patientCount = connections.patientCount;
            for (int i = 0; i < connections.patientCount; ++i) {
                status_.patients[i] = connections.patients[i];
            }
            if (connections.patientCount == 0) {
                out.reason = "no one followed";
                return out;
            }
            if (connections.chosen < 0) {
                out.reason = "choose a patient";
                return out;
            }
            librePatientId_ = connections.patients[connections.chosen].id;
            pendingCurrent_ = connections.current;
            if (pendingCurrent_.sgv < 1 || pendingCurrent_.epoch <= 0) {
                out.reason = "no data";
                return out;
            }
            // History only when there is a gap behind the latest reading; once
            // filled, each minute's reading is merged and the graph not asked.
            if (newestEpoch() < pendingCurrent_.epoch - 6 * 60) {
                out.kind = Outcome::Kind::Next;
                out.next = Step::LibreGraph;
                return out;
            }
            sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, pendingCurrent_,
                                       kCloudSampleSpacingSeconds);
            out.kind = Outcome::Kind::Samples;
            out.count = sampleCount_;
            return out;
        }
        case Step::LibreGraph: {
            int count = -1;
            if (body.size() < kMaxGraphBodyBytes) {
                count = parseLibreGraph(body, incoming_, kMaxHistory, tokens_, kTokenCapacity);
            }
            // A graph that will not read still leaves the latest reading.
            if (count > 0) {
                for (int i = 0; i < count; ++i) {
                    sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, incoming_[i],
                                               kCloudSampleSpacingSeconds);
                }
            }
            sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, pendingCurrent_,
                                       kCloudSampleSpacingSeconds);
            out.kind = Outcome::Kind::Samples;
            out.count = sampleCount_;
            return out;
        }
        default:
            return out;
    }
}

GlucoseSource::Outcome GlucoseSource::answerMedtrum(int httpStatus, std::string_view body,
                                                    std::string_view cookies) {
    Outcome out;
    static char reason[24];
    if (step_ != Step::MedtrumLogin && (httpStatus == 401 || httpStatus == 403) &&
        !renewedThisPoll_) {
        renewedThisPoll_ = true;
        medtrumCookie_.clear();
        out.kind = Outcome::Kind::Next;
        out.next = Step::MedtrumLogin;
        return out;
    }
    if (refused(httpStatus)) {
        out.reason = httpStatus == 429 ? "too many requests" : "wrong password";
        out.fatal = true;
        dropSession();
        return out;
    }
    if (httpStatus != 200) {
        std::snprintf(reason, sizeof reason, "http %d", httpStatus);
        out.reason = reason;
        return out;
    }

    switch (step_) {
        case Step::MedtrumLogin:
            if (!medtrumOk(body, tokens_, kTokenCapacity)) {
                out.reason = "wrong password";
                out.fatal = true;
                return out;
            }
            if (cookies.empty()) {
                // Held off like a refusal: logging in again every minute would
                // be the hammering the hold exists to prevent.
                out.reason = "no session cookie";
                out.fatal = true;
                return out;
            }
            medtrumCookie_.assign(cookies);
            out.kind = Outcome::Kind::Next;
            out.next = Step::MedtrumMonitor;
            return out;
        case Step::MedtrumMonitor: {
            Sample current;
            std::string user;
            if (!parseMedtrumMonitor(body, current, user, tokens_, kTokenCapacity)) {
                // An OK answer with nothing in it is a sensor warming up or
                // expired, not a session problem: no login for that.
                if (medtrumOk(body, tokens_, kTokenCapacity)) {
                    out.reason = "no data";
                    return out;
                }
                // An expired session answers 200 with res != OK: log in once.
                if (!renewedThisPoll_) {
                    renewedThisPoll_ = true;
                    medtrumCookie_.clear();
                    out.kind = Outcome::Kind::Next;
                    out.next = Step::MedtrumLogin;
                    return out;
                }
                out.reason = "bad data";
                return out;
            }
            medtrumUser_ = user;
            pendingCurrent_ = current;
            if (newestEpoch() < current.epoch - 6 * 60) {
                const std::int64_t backfill = nowUnix_ - kDexcomMinutes * 60;
                medtrumFrom_ = newestEpoch() > backfill ? newestEpoch() + 1 : backfill;
                out.kind = Outcome::Kind::Next;
                out.next = Step::MedtrumHistory;
                return out;
            }
            sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, current,
                                       kCloudSampleSpacingSeconds);
            out.kind = Outcome::Kind::Samples;
            out.count = sampleCount_;
            return out;
        }
        case Step::MedtrumHistory: {
            const int count =
                parseMedtrumHistory(body, incoming_, kMaxHistory, tokens_, kTokenCapacity);
            for (int i = 0; i < count; ++i) {
                sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, incoming_[i],
                                           kCloudSampleSpacingSeconds);
            }
            // The next hour, while there is one and steps are left.
            medtrumFrom_ += 3601;
            if (count >= 0 && medtrumFrom_ < nowUnix_ && stepsThisPoll_ < kMaxStepsPerPoll) {
                out.kind = Outcome::Kind::Next;
                out.next = Step::MedtrumHistory;
                return out;
            }
            sampleCount_ = mergeSample(samples_, sampleCount_, kMaxHistory, pendingCurrent_,
                                       kCloudSampleSpacingSeconds);
            out.kind = Outcome::Kind::Samples;
            out.count = sampleCount_;
            return out;
        }
        default:
            return out;
    }
}

// --- outcomes -----------------------------------------------------------------

void GlucoseSource::succeed(std::uint64_t nowMillis, const Outcome& outcome) {
    if (outcome.replace) {
        for (int i = 0; i < outcome.count; ++i) {
            samples_[i] = incoming_[i];
        }
        sampleCount_ = outcome.count;
    }
    status_.sampleCount = sampleCount_;
    status_.lastSuccessMillis = nowMillis;
    status_.lastFailure[0] = '\0';
    status_.fatalStreak = 0;
    status_.holdUntilMillis = 0;
    dueMillis_ = nowMillis + periodMillis(effectivePollSeconds());
    dirty_ = true;
}

void GlucoseSource::fail(const char* reason, std::uint64_t nowMillis, bool fatal) {
    ++status_.failures;
    std::snprintf(status_.lastFailure, sizeof status_.lastFailure, "%s", reason);
    const std::uint64_t period = periodMillis(effectivePollSeconds());
    if (fatal) {
        if (status_.fatalStreak < 8) {
            ++status_.fatalStreak;
        }
        const std::uint64_t base = period > kFatalHoldMinMillis ? period : kFatalHoldMinMillis;
        const unsigned shift = status_.fatalStreak > 3 ? 3u
                                                       : static_cast<unsigned>(status_.fatalStreak - 1);
        const std::uint64_t hold = base << shift;
        status_.holdUntilMillis = nowMillis + (hold > kFatalHoldMaxMillis ? kFatalHoldMaxMillis : hold);
        dueMillis_ = status_.holdUntilMillis;
    } else {
        dueMillis_ = nowMillis + period;
    }
}

void GlucoseSource::rebuild(std::int64_t nowUnix, bool wallClockValid, int utcOffsetSeconds) {
    reading_ = readingFromSamples(samples_, sampleCount_, nowUnix, utcOffsetSeconds, wallClockValid);
    ++revision_;
    dirty_ = false;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
