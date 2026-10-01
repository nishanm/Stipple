// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/api/Http.h"

#include <vector>

#include "stipple/api/JsonWriter.h"

namespace stipple {
namespace api {
namespace {

/// Split a path into non-empty segments, so "/api/v1/apps/" and "/api/v1/apps"
/// route identically. A trailing slash is a formatting choice, not a different
/// resource.
std::vector<std::string_view> segments(std::string_view path) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;

    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start) {
            parts.push_back(path.substr(start, end - start));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

}  // namespace

Method methodFromName(std::string_view name) noexcept {
    if (name == "GET") return Method::Get;
    if (name == "POST") return Method::Post;
    if (name == "PUT") return Method::Put;
    if (name == "PATCH") return Method::Patch;
    if (name == "DELETE") return Method::Delete;
    if (name == "OPTIONS") return Method::Options;
    if (name == "HEAD") return Method::Head;
    return Method::Unknown;
}

const char* methodName(Method method) noexcept {
    switch (method) {
        case Method::Get: return "GET";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Patch: return "PATCH";
        case Method::Delete: return "DELETE";
        case Method::Options: return "OPTIONS";
        case Method::Head: return "HEAD";
        case Method::Unknown: break;
    }
    return "UNKNOWN";
}

bool Request::hasQuery(std::string_view name) const {
    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::size_t end = amp == std::string::npos ? query.size() : amp;
        const std::string_view pair(query.data() + start, end - start);

        const std::size_t equals = pair.find('=');
        const std::string_view key = equals == std::string_view::npos ? pair : pair.substr(0, equals);
        if (key == name) {
            return true;
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return false;
}

std::string Request::queryValue(std::string_view name, std::string_view fallback) const {
    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::size_t end = amp == std::string::npos ? query.size() : amp;
        const std::string_view pair(query.data() + start, end - start);

        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            return std::string(pair.substr(equals + 1));
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return std::string(fallback);
}

// --- responses ---------------------------------------------------------------

Response ok(std::string body) {
    Response response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

Response created(std::string body) {
    Response response;
    response.status = 201;
    response.body = std::move(body);
    return response;
}

Response noContent() {
    Response response;
    response.status = 204;
    response.body.clear();
    return response;
}

Response error(int status, std::string_view code, std::string_view message) {
    JsonWriter writer;
    writer.beginObject()
        .key("error")
        .beginObject()
        .member("code", code)
        .member("message", message)
        .endObject()
        .endObject();

    Response response;
    response.status = status;
    response.body = writer.take();
    return response;
}

Response badRequest(std::string_view message) {
    return error(400, "bad_request", message);
}

Response unauthorized(std::string_view message) {
    return error(401, "unauthorized", message);
}

Response notFound(std::string_view message) {
    return error(404, "not_found", message);
}

Response methodNotAllowed(std::string_view message) {
    return error(405, "method_not_allowed", message);
}

Response conflict(std::string_view message) {
    return error(409, "conflict", message);
}

Response payloadTooLarge(std::string_view message) {
    return error(413, "payload_too_large", message);
}

Response unprocessable(std::string_view message) {
    return error(422, "unprocessable", message);
}

Response serverError(std::string_view message) {
    return error(500, "internal_error", message);
}

// --- routing -----------------------------------------------------------------

RouteMatch matchRoute(std::string_view path) {
    RouteMatch match;

    const std::vector<std::string_view> parts = segments(path);
    if (parts.size() < 3 || parts[0] != "api" || parts[1] != "v1") {
        return match;
    }

    const std::string_view head = parts[2];

    if (parts.size() == 3) {
        if (head == "device") match.resource = Resource::Device;
        else if (head == "health") match.resource = Resource::Health;
        else if (head == "version") match.resource = Resource::Version;
        else if (head == "diagnostics") match.resource = Resource::Diagnostics;
        else if (head == "logs") match.resource = Resource::Logs;
        else if (head == "apps") match.resource = Resource::AppCollection;
        else if (head == "notifications") match.resource = Resource::NotificationCollection;
        else if (head == "assets") match.resource = Resource::AssetCollection;
        else if (head == "scripts") match.resource = Resource::ScriptCollection;
        else if (head == "settings") match.resource = Resource::Settings;
        else if (head == "input") match.resource = Resource::Input;
        else if (head == "sound") match.resource = Resource::Sound;
        else if (head == "network") match.resource = Resource::Network;
        return match;
    }

    if (parts.size() == 5 && head == "glucose" && parts[3] == "alarm" && parts[4] == "test") {
        match.resource = Resource::GlucoseAlarmTest;
        return match;
    }

    if (parts.size() == 4) {
        if (head == "system" && parts[3] == "reboot") {
            match.resource = Resource::SystemReboot;
            return match;
        }
        if (head == "system" && parts[3] == "firmware") {
            match.resource = Resource::SystemFirmware;
            return match;
        }
        if (head == "system" && parts[3] == "reset") {
            match.resource = Resource::SystemReset;
            return match;
        }
        if (head == "network" && parts[3] == "scan") {
            match.resource = Resource::NetworkScan;
            return match;
        }
        if (head == "network" && parts[3] == "join") {
            match.resource = Resource::NetworkJoin;
            return match;
        }
        if (head == "network" && parts[3] == "remember") {
            match.resource = Resource::NetworkRemember;
            return match;
        }
        if (head == "network" && parts[3] == "forget") {
            match.resource = Resource::NetworkForget;
            return match;
        }
        if (head == "display" && parts[3] == "frame") {
            match.resource = Resource::DisplayFrame;
            return match;
        }
        if (head == "apps") {
            match.resource = Resource::AppItem;
            match.id = std::string(parts[3]);
            return match;
        }
        if (head == "notifications") {
            match.resource = Resource::NotificationItem;
            match.id = std::string(parts[3]);
            return match;
        }
        if (head == "assets") {
            match.resource = Resource::AssetItem;
            match.id = std::string(parts[3]);
            return match;
        }
        if (head == "scripts") {
            match.resource = Resource::ScriptItem;
            match.id = std::string(parts[3]);
            return match;
        }
        return match;
    }

    if (parts.size() == 5 && head == "apps" && parts[4] == "activate") {
        match.resource = Resource::AppActivate;
        match.id = std::string(parts[3]);
        return match;
    }

    return match;
}

}  // namespace api
}  // namespace stipple
