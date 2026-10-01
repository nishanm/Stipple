// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseCloud.h"

#include <cstring>

#include "stipple/apps/GlucoseSource.h"

namespace stipple {
namespace apps {
namespace glucose {
namespace {

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

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Days from 1970-01-01 to the civil date (Howard Hinnant's algorithm).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(year + (m <= 2 ? 1 : 0));
}

void appendTwo(std::string& out, unsigned value) {
    out += static_cast<char>('0' + (value / 10) % 10);
    out += static_cast<char>('0' + value % 10);
}

/// "YYYY-MM-DD%20HH:MM:SS", the form Medtrum's history query wants.
std::string medtrumTime(std::int64_t unix) {
    const std::int64_t days = unix >= 0 ? unix / 86400 : (unix - 86399) / 86400;
    const std::int64_t secs = unix - days * 86400;
    int y = 1970;
    unsigned m = 1;
    unsigned d = 1;
    civilFromDays(days, y, m, d);
    std::string out = std::to_string(y);
    out += '-';
    appendTwo(out, m);
    out += '-';
    appendTwo(out, d);
    out += "%20";
    appendTwo(out, static_cast<unsigned>(secs / 3600));
    out += ':';
    appendTwo(out, static_cast<unsigned>((secs % 3600) / 60));
    out += ':';
    appendTwo(out, static_cast<unsigned>(secs % 60));
    return out;
}

/// A JSON string value into a fixed buffer, truncated, NUL-terminated. Escape
/// sequences are copied as written; these are ids and display names.
template <std::size_t N>
void copyTo(char (&out)[N], std::string_view text) noexcept {
    const std::size_t n = text.size() < N - 1 ? text.size() : N - 1;
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

/// A sample from Medtrum's or Dexcom's number, or none.
bool plausible(int sgv) noexcept { return sgv >= 1 && sgv <= 1000; }

/// Milliseconds or seconds to seconds: anything past 2286 in seconds is
/// milliseconds.
std::int64_t toSeconds(std::int64_t value) noexcept {
    return value > 10'000'000'000LL ? value / 1000 : value;
}

}  // namespace

// --- which service ------------------------------------------------------------

const char* sourceKindName(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::Dexcom: return "dexcom";
        case SourceKind::LibreLinkUp: return "librelinkup";
        case SourceKind::Medtrum: return "medtrum";
        case SourceKind::Nightscout: break;
    }
    return "nightscout";
}

bool sourceKindFromName(std::string_view name, SourceKind& out) noexcept {
    for (const SourceKind kind : {SourceKind::Nightscout, SourceKind::Dexcom,
                                  SourceKind::LibreLinkUp, SourceKind::Medtrum}) {
        if (name == sourceKindName(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

// --- shared helpers -----------------------------------------------------------

std::string jsonQuoted(std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    out += "\\u00";
                    out += kHex[byte >> 4];
                    out += kHex[byte & 0x0f];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string formEncoded(std::string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c) ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0f];
        }
    }
    return out;
}

int mergeSample(Sample* samples, int count, int capacity, const Sample& sample,
                std::int64_t spacingSeconds) noexcept {
    if (capacity <= 0 || !plausible(sample.sgv) || sample.epoch <= 0) {
        return count;
    }
    // Newer than the newest, but too close to it: it replaces the newest, so
    // the latest value is always shown and the history stays spaced.
    if (count > 1 && sample.epoch > samples[count - 1].epoch &&
        sample.epoch - samples[count - 2].epoch < spacingSeconds) {
        const Trend trend = sample.trend == Trend::None ? samples[count - 1].trend : sample.trend;
        samples[count - 1] = sample;
        samples[count - 1].trend = trend;
        return count;
    }
    for (int i = 0; i < count; ++i) {
        const std::int64_t gap = samples[i].epoch - sample.epoch;
        if (gap > -30 && gap < 30) {
            const Trend trend = sample.trend == Trend::None ? samples[i].trend : sample.trend;
            samples[i].sgv = sample.sgv;
            samples[i].trend = trend;
            return count;
        }
    }
    if (count == capacity) {
        if (sample.epoch <= samples[0].epoch) {
            return count;  // older than everything kept
        }
        for (int i = 1; i < count; ++i) {
            samples[i - 1] = samples[i];
        }
        --count;
    }
    int at = count;
    while (at > 0 && samples[at - 1].epoch > sample.epoch) {
        samples[at] = samples[at - 1];
        --at;
    }
    samples[at] = sample;
    return count + 1;
}

// --- Dexcom Share -------------------------------------------------------------

const char* dexcomBaseUrl(std::string_view server) noexcept {
    if (server == "us") return "https://share1.dexcom.com";
    if (server == "ous") return "https://shareous1.dexcom.com";
    if (server == "jp") return "https://share.dexcom.jp";
    return nullptr;
}

const char* dexcomApplicationId(std::string_view server) noexcept {
    return server == "jp" ? "d8665ade-9673-4e27-9ff6-92db4ce13d13"
                          : "d89443d2-327c-4a6f-89e5-496bbb0317db";
}

std::string dexcomAccountBody(std::string_view username, std::string_view password,
                              std::string_view server) {
    std::string body = "{\"accountName\":";
    body += jsonQuoted(username);
    body += ",\"password\":";
    body += jsonQuoted(password);
    body += ",\"applicationId\":";
    body += jsonQuoted(dexcomApplicationId(server));
    body += '}';
    return body;
}

std::string dexcomSessionBody(std::string_view accountId, std::string_view password,
                              std::string_view server) {
    std::string body = "{\"accountId\":";
    body += jsonQuoted(accountId);
    body += ",\"password\":";
    body += jsonQuoted(password);
    body += ",\"applicationId\":";
    body += jsonQuoted(dexcomApplicationId(server));
    body += '}';
    return body;
}

std::string dexcomQuotedId(std::string_view body) {
    while (!body.empty() && (body.front() == ' ' || body.front() == '\n' || body.front() == '\r')) {
        body.remove_prefix(1);
    }
    while (!body.empty() && (body.back() == ' ' || body.back() == '\n' || body.back() == '\r')) {
        body.remove_suffix(1);
    }
    if (body.size() < 3 || body.front() != '"' || body.back() != '"') {
        return {};
    }
    const std::string_view id = body.substr(1, body.size() - 2);
    if (id.size() > 64) {
        return {};
    }
    bool allZero = true;
    for (const char c : id) {
        const bool hex = isDigit(c) || (lower(c) >= 'a' && lower(c) <= 'f');
        if (!hex && c != '-') {
            return {};
        }
        if (c != '0' && c != '-') {
            allZero = false;
        }
    }
    return allZero ? std::string() : std::string(id);
}

bool dexcomCredentialsRejected(std::string_view body) noexcept {
    for (const std::string_view marker :
         {std::string_view("AccountPasswordInvalid"),
          std::string_view("SSO_AuthenticateAccountNotFound"),
          std::string_view("SSO_AuthenticatePasswordInvalid"),
          std::string_view("SSO_AuthenticateMaxAttemptsExceed"),
          std::string_view("SSO_InternalError")}) {
        if (body.find(marker) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool dexcomSessionExpired(std::string_view body) noexcept {
    return body.find("Session") != std::string_view::npos;
}

std::int64_t dexcomDateSeconds(std::string_view text) noexcept {
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos) {
        return 0;
    }
    std::int64_t millis = 0;
    std::size_t digits = 0;
    for (std::size_t i = open + 1; i < text.size() && isDigit(text[i]); ++i) {
        millis = millis * 10 + (text[i] - '0');
        if (++digits > 15) {
            return 0;
        }
    }
    return digits >= 10 ? toSeconds(millis) : 0;
}

int parseDexcomReadings(std::string_view body, Sample* out, int capacity, json::Token* tokens,
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
    for (int i = 0; i < root.size(); ++i) {
        const json::Value entry = root[i];
        if (!entry.isObject()) {
            continue;
        }
        Sample sample;
        sample.sgv = static_cast<int>(entry["Value"].toInt(-1));
        const json::Value when = entry["ST"].valid() ? entry["ST"] : entry["WT"];
        sample.epoch = when.isString() ? dexcomDateSeconds(when.raw()) : 0;
        const json::Value trend = entry["Trend"];
        if (trend.isString()) {
            sample.trend = trendFromDirection(trend.raw());
        } else if (trend.isNumber()) {
            sample.trend = trendFromDirectionIndex(trend.toInt(0));
        }
        // Not mergeSample: Dexcom's twins eleven seconds apart are part of
        // what the delta rule expects to see, and each answer is complete.
        if (!plausible(sample.sgv) || sample.epoch <= 0) {
            continue;
        }
        if (count == capacity) {
            if (sample.epoch <= out[0].epoch) {
                continue;
            }
            for (int k = 1; k < count; ++k) {
                out[k - 1] = out[k];
            }
            --count;
        }
        int at = count;
        while (at > 0 && out[at - 1].epoch > sample.epoch) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = sample;
        ++count;
    }
    return count;
}

// --- LibreLinkUp --------------------------------------------------------------

const char* libreHost(std::string_view region) noexcept {
    struct Entry {
        const char* region;
        const char* host;
    };
    static const Entry kHosts[] = {
        {"AE", "api-ae.libreview.io"}, {"AP", "api-ap.libreview.io"},
        {"AU", "api-au.libreview.io"}, {"CA", "api-ca.libreview.io"},
        {"DE", "api-de.libreview.io"}, {"EU", "api-eu.libreview.io"},
        {"EU2", "api-eu2.libreview.io"}, {"FR", "api-fr.libreview.io"},
        {"JP", "api-jp.libreview.io"}, {"US", "api-us.libreview.io"},
        {"LA", "api-la.libreview.io"}, {"RU", "api.libreview.ru"},
    };
    for (const Entry& entry : kHosts) {
        if (equalsIgnoringCase(region, entry.region)) {
            return entry.host;
        }
    }
    return nullptr;
}

std::string libreLoginBody(std::string_view email, std::string_view password) {
    std::string body = "{\"email\":";
    body += jsonQuoted(email);
    body += ",\"password\":";
    body += jsonQuoted(password);
    body += '}';
    return body;
}

bool parseLibreLogin(std::string_view body, LibreLogin& out, json::Token* tokens,
                     int tokenCapacity) {
    out = LibreLogin{};
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return false;
    }
    const json::Value root = document.root();
    if (!root.isObject() || !root["status"].isNumber()) {
        return false;
    }
    out.status = static_cast<int>(root["status"].toInt(-1));
    const json::Value data = root["data"];
    if (data["redirect"].toBool(false)) {
        out.redirect = true;
        out.region = data["region"].toString();
        for (char& c : out.region) {
            c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        }
        return true;
    }
    out.token = data["authTicket"]["token"].toString();
    out.expires = data["authTicket"]["expires"].toInt(0);
    out.userId = data["user"]["id"].toString();
    return true;
}

std::int64_t libreTimestampSeconds(std::string_view text) noexcept {
    // M/D/YYYY h:mm:ss AM
    unsigned field[6] = {};
    std::size_t f = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < text.size() && f < 6) {
        if (isDigit(text[i])) {
            field[f] = field[f] * 10 + static_cast<unsigned>(text[i] - '0');
            if (field[f] > 9999) {
                return 0;
            }
            any = true;
        } else if (any) {
            ++f;
            any = false;
            if (f == 6) {
                break;
            }
        }
        ++i;
    }
    if (any && f < 6) {
        ++f;
    }
    if (f < 6) {
        return 0;
    }
    const unsigned month = field[0];
    const unsigned day = field[1];
    const std::int64_t year = field[2];
    unsigned hour = field[3];
    const unsigned minute = field[4];
    const unsigned second = field[5];
    const bool pm = text.find("PM") != std::string_view::npos;
    const bool am = text.find("AM") != std::string_view::npos;
    if (pm && hour < 12) {
        hour += 12;
    } else if (am && hour == 12) {
        hour = 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 2000 || hour > 23 ||
        minute > 59 || second > 60) {
        return 0;
    }
    return daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

Trend libreTrend(std::int64_t arrow) noexcept {
    switch (arrow) {
        case 5: return Trend::SingleUp;
        case 4: return Trend::FortyFiveUp;
        case 3: return Trend::Flat;
        case 2: return Trend::FortyFiveDown;
        case 1: return Trend::SingleDown;
        default: return Trend::None;
    }
}

namespace {

Sample libreMeasurement(const json::Value& measurement) {
    Sample sample;
    sample.sgv = static_cast<int>(measurement["ValueInMgPerDl"].toInt(-1));
    sample.epoch = libreTimestampSeconds(measurement["FactoryTimestamp"].raw());
    if (measurement["TrendArrow"].isNumber()) {
        sample.trend = libreTrend(measurement["TrendArrow"].toInt(0));
    }
    return sample;
}

}  // namespace

bool parseLibreConnections(std::string_view body, std::string_view wantedId,
                           LibreConnections& out, json::Token* tokens, int tokenCapacity) {
    out = LibreConnections{};
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return false;
    }
    const json::Value root = document.root();
    if (!root.isObject() || !root["status"].isNumber()) {
        return false;
    }
    out.status = static_cast<int>(root["status"].toInt(-1));
    const json::Value data = root["data"];
    if (!data.isArray()) {
        return out.status != 0;  // an error answer is still an answer
    }
    int wanted = -1;
    for (int i = 0; i < data.size(); ++i) {
        const json::Value person = data[i];
        const std::string_view id = person["patientId"].raw();
        if (!wantedId.empty() && id == wantedId) {
            wanted = i;
        }
        if (out.patientCount < kMaxLibrePatients) {
            LibrePatient& patient = out.patients[out.patientCount++];
            copyTo(patient.id, id);
            std::string name = person["firstName"].toString();
            const std::string last = person["lastName"].toString();
            if (!last.empty()) {
                name += ' ';
                name += last;
            }
            copyTo(patient.name, name);
        }
    }
    // The only person followed, but only when nobody was named: a named person
    // who is no longer followed is not silently replaced by somebody else.
    if (wanted < 0 && wantedId.empty() && data.size() == 1) {
        wanted = 0;
    }
    if (wanted >= 0) {
        out.chosen = wanted < kMaxLibrePatients ? wanted : -1;
        out.current = libreMeasurement(data[wanted]["glucoseMeasurement"]);
        if (out.chosen < 0) {
            // Followed, but past the four kept for the picker: still read it,
            // and name it properly in the last slot.
            out.chosen = kMaxLibrePatients - 1;
            const json::Value person = data[wanted];
            copyTo(out.patients[out.chosen].id, person["patientId"].raw());
            std::string name = person["firstName"].toString();
            const std::string last = person["lastName"].toString();
            if (!last.empty()) {
                name += ' ';
                name += last;
            }
            copyTo(out.patients[out.chosen].name, name);
        }
    }
    return true;
}

int parseLibreGraph(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                    int tokenCapacity) noexcept {
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return -1;
    }
    const json::Value root = document.root();
    if (!root.isObject() || root["status"].toInt(-1) != 0) {
        return -1;
    }
    int count = 0;
    const json::Value graph = root["data"]["graphData"];
    for (int i = 0; graph.isArray() && i < graph.size(); ++i) {
        count = mergeSample(out, count, capacity, libreMeasurement(graph[i]));
    }
    const json::Value latest = root["data"]["connection"]["glucoseMeasurement"];
    if (latest.isObject()) {
        count = mergeSample(out, count, capacity, libreMeasurement(latest));
    }
    return count;
}

// --- Medtrum EasyView ---------------------------------------------------------

std::string medtrumLoginBody(std::string_view email, std::string_view password) {
    std::string body = "apptype=Follow&user_name=";
    body += formEncoded(email);
    body += "&password=";
    body += formEncoded(password);
    body += "&platform=google&user_type=M";
    return body;
}

std::string medtrumHistoryUrl(std::int64_t fromUnix, std::int64_t toUnix,
                              std::string_view username) {
    std::string url(kMedtrumHistoryUrl);
    url += "&st=";
    url += medtrumTime(fromUnix);
    url += "&et=";
    url += medtrumTime(toUnix);
    url += "&user_name=";
    url += formEncoded(username);
    return url;
}

int medtrumMgdl(double value) noexcept {
    const double mgdl = value < 30.0 ? value * 18.0 : value;
    return static_cast<int>(mgdl);
}

Trend medtrumTrend(std::int64_t rate) noexcept {
    switch (rate) {
        case 0:
        case 8: return Trend::Flat;
        case 1: return Trend::FortyFiveUp;
        case 2: return Trend::SingleUp;
        case 3: return Trend::DoubleUp;
        case 4: return Trend::FortyFiveDown;
        case 5: return Trend::SingleDown;
        case 6: return Trend::DoubleDown;
        default: return Trend::None;
    }
}

bool medtrumOk(std::string_view body, json::Token* tokens, int tokenCapacity) {
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return false;
    }
    return document.root()["res"].stringEquals("OK");
}

bool parseMedtrumMonitor(std::string_view body, Sample& current, std::string& username,
                         json::Token* tokens, int tokenCapacity) {
    current = Sample{};
    username.clear();
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return false;
    }
    const json::Value root = document.root();
    if (!root["res"].stringEquals("OK")) {
        return false;
    }
    const json::Value first = root["monitorlist"][0];
    if (!first.isObject()) {
        return false;
    }
    const json::Value status = first["sensor_status"];
    current.sgv = medtrumMgdl(status["glucose"].toDouble(-1.0));
    current.trend = medtrumTrend(status["glucoseRate"].toInt(-1));
    current.epoch = toSeconds(status["updateTime"].toInt(0));
    username = first["username"].toString();
    return plausible(current.sgv) && current.epoch > 0 && !username.empty();
}

int parseMedtrumHistory(std::string_view body, Sample* out, int capacity, json::Token* tokens,
                        int tokenCapacity) noexcept {
    json::Document document(tokens, tokenCapacity);
    if (document.parse(body) != json::Error::None) {
        return -1;
    }
    const json::Value root = document.root();
    if (!root["res"].stringEquals("OK")) {
        return -1;
    }
    int count = 0;
    const json::Value rows = root["data"];
    for (int i = 0; rows.isArray() && i < rows.size(); ++i) {
        const json::Value row = rows[i];
        Sample sample;
        sample.epoch = toSeconds(row[1].toInt(0));
        sample.sgv = medtrumMgdl(row[3].toDouble(-1.0));
        count = mergeSample(out, count, capacity, sample);
    }
    return count;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
