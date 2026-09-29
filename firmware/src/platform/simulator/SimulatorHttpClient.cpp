// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/simulator/SimulatorHttpClient.h"

#include <utility>

#include "stipple/net/HttpFetch.h"

namespace stipple {
namespace platform {
namespace simulator {

void SimulatorHttpClient::answer(Route route) {
    for (Route& held : routes_) {
        if (held.url == route.url) {
            held = std::move(route);
            return;
        }
    }
    routes_.push_back(std::move(route));
}

const SimulatorHttpClient::Route* SimulatorHttpClient::findRoute(
    std::string_view url) const noexcept {
    for (const Route& route : routes_) {
        if (route.url == url) {
            return &route;
        }
    }
    return nullptr;
}

bool SimulatorHttpClient::begin(const HttpRequest& request) {
    if (stage_ == Stage::Running) {
        return false;
    }

    const std::string_view url = request.url;

    // Parsed even though nothing here connects, so the simulator refuses the
    // same URLs the device would. A script that worked in the emulator and
    // failed on the panel because of a URL this did not check would be the
    // emulator lying, which is the one thing it must never do.
    net::http::Url parsed;
    if (!net::http::parseUrl(url, parsed)) {
        stage_ = Stage::Failed;
        failure_ = "bad url";
        return false;
    }
    if (!net::http::headerIsSafe(request.headerName, request.headerValue)) {
        stage_ = Stage::Failed;
        failure_ = "bad header";
        return false;
    }

    ++requests_;
    asked_.emplace_back(url);
    std::string header;
    if (!request.headerName.empty()) {
        header.append(request.headerName);
        header += ": ";
        header.append(request.headerValue);
    }
    askedHeaders_.push_back(std::move(header));
    maxBodyBytes_ = request.maxBodyBytes;
    truncated_ = false;

    const Route* route = findRoute(url);
    if (route == nullptr) {
        pendingStatus_ = defaultStatus_;
        pendingBody_ = defaultBody_;
        pendingFailure_ = defaultFailure_;
        hanging_ = false;
        readyAtMillis_ = 0;
        latencyMillis_ = 0;
        stage_ = Stage::Running;
        return true;
    }

    pendingStatus_ = route->status;
    pendingBody_ = route->body;
    pendingFailure_ = route->failure;
    hanging_ = route->hang;
    readyAtMillis_ = 0;
    latencyMillis_ = route->latencyMillis;
    stage_ = Stage::Running;
    return true;
}

void SimulatorHttpClient::poll(std::uint64_t nowMillis) {
    if (stage_ != Stage::Running) {
        return;
    }
    if (hanging_) {
        return;  // never finishes, on purpose
    }
    if (readyAtMillis_ == 0) {
        readyAtMillis_ = nowMillis + latencyMillis_;
        return;
    }
    if (nowMillis < readyAtMillis_) {
        return;
    }

    if (!pendingFailure_.empty()) {
        failure_ = pendingFailure_;
        status_ = 0;
        body_.clear();
        stage_ = Stage::Failed;
        return;
    }

    status_ = pendingStatus_;
    if (pendingBody_.size() > maxBodyBytes_) {
        body_ = pendingBody_.substr(0, maxBodyBytes_);
        truncated_ = true;
    } else {
        body_ = pendingBody_;
    }
    failure_.clear();
    stage_ = Stage::Done;
}

void SimulatorHttpClient::reset() {
    stage_ = Stage::Idle;
    status_ = 0;
    body_.clear();
    failure_.clear();
    readyAtMillis_ = 0;
    hanging_ = false;
}

}  // namespace simulator
}  // namespace platform
}  // namespace stipple
