// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002Platform.h"

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <algorithm>
#include <functional>
#include <vector>
#include <cstring>
#include <string>
#include <utility>

#include "stipple/platform/tc002/WirelessStats.h"
#include "stipple/platform/tc002/WpaCommands.h"
#include "stipple/platform/tc002/WpaReplies.h"

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

/// Run a command to completion, saying nothing.
///
/// Deliberately not `system()`: this process owns the panel, and a shell
/// inheriting its stdout would put vendor chatter into the log the web UI
/// shows.
void runQuietly(const char* const argv[]) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, STDOUT_FILENO);
            ::dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO) {
                ::close(null);
            }
        }
        ::execv(argv[0], const_cast<char* const*>(argv));
        ::_exit(127);
    }
    ::waitpid(pid, nullptr, 0);
}

/// Same source as Tc002Input's timestamps, and it has to stay that way:
/// InputMapper subtracts one from the other to get press duration, and two
/// different clocks would make a long press come out negative.
std::uint64_t monotonicNowMillis() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

std::int64_t realtimeNowSeconds() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec);
}

/// A key is usable as a filename only if it is plainly one. No dots-only names,
/// no separators, nothing that could climb out of the directory.
bool isSafeKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 64) {
        return false;
    }
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    // ".." would be caught by the separator rule above only in company with a
    // slash; reject it outright so no caller can rely on that coincidence.
    return key != "." && key != "..";
}

/// fsync a directory so a rename is durable, not just visible.
void syncDirectory(const std::string& path) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

}  // namespace

// --- clock ------------------------------------------------------------------

std::uint64_t Tc002Clock::monotonicMillis() const { return monotonicNowMillis(); }

bool Tc002Clock::wallClockValid() const {
    return realtimeNowSeconds() >= kPlausibleEpoch;
}

std::int64_t Tc002Clock::unixSeconds() const { return realtimeNowSeconds(); }

// --- storage ----------------------------------------------------------------

Tc002Storage::Tc002Storage(std::string directory) : directory_(std::move(directory)) {}

bool Tc002Storage::open() {
    if (::mkdir(directory_.c_str(), 0755) != 0) {
        // Already existing is the normal case after the first boot.
        struct stat info;
        if (::stat(directory_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            return false;
        }
    }
    return ::access(directory_.c_str(), R_OK | W_OK | X_OK) == 0;
}

std::string Tc002Storage::pathFor(std::string_view key) const {
    if (!isSafeKey(key)) {
        return std::string();
    }
    std::string path = directory_;
    path += '/';
    path.append(key);
    return path;
}

bool Tc002Storage::exists(std::string_view key) const {
    const std::string path = pathFor(key);
    if (path.empty()) {
        return false;
    }
    struct stat info;
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool Tc002Storage::read(std::string_view key, std::string& out) const {
    out.clear();

    const std::string path = pathFor(key);
    if (path.empty()) {
        return false;
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    char chunk[1024];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            ::close(fd);
            out.clear();
            return false;
        }
        if (got == 0) {
            break;
        }
        // Bounded: a file larger than the limit is corrupt or hostile, and
        // reading it into memory is exactly what §38 forbids.
        if (out.size() + static_cast<std::size_t>(got) > kMaxValueBytes) {
            ::close(fd);
            out.clear();
            return false;
        }
        out.append(chunk, static_cast<std::size_t>(got));
    }

    ::close(fd);
    return true;
}

bool Tc002Storage::write(std::string_view key, std::string_view value) {
    if (value.size() > kMaxValueBytes) {
        return false;
    }

    const std::string path = pathFor(key);
    if (path.empty()) {
        return false;
    }

    // Write, flush, rename. The temporary lives in the same directory so the
    // rename stays within one filesystem and is therefore atomic.
    const std::string temporary = path + ".tmp";

    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    std::size_t written = 0;
    while (written < value.size()) {
        const ssize_t got =
            ::write(fd, value.data() + written, value.size() - written);
        if (got <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(got);
    }

    // Without this the rename can land before the data does, and a power cut
    // between them leaves a file that exists and is empty - which is worse than
    // one that was never written, because it looks like valid stored state.
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }
    ::close(fd);

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }

    syncDirectory(directory_);
    return true;
}

bool Tc002Storage::remove(std::string_view key) {
    const std::string path = pathFor(key);
    if (path.empty()) {
        return false;
    }
    if (::unlink(path.c_str()) != 0) {
        return false;
    }
    syncDirectory(directory_);
    return true;
}

// --- network ----------------------------------------------------------------

namespace {

/// Read a small file whole. Returns empty on any failure, which every caller
/// here treats as "this platform cannot say" rather than as an error.
std::string readSmallFile(const char* path) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return std::string();
    }
    std::string out;
    char chunk[512];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        out.append(chunk, static_cast<std::size_t>(got));
        // Bounded, per §38. /proc/net/wireless is a few hundred bytes; a file
        // that keeps producing is not the file this was looking for.
        if (out.size() > 8192) {
            break;
        }
    }
    ::close(fd);
    return out;
}

/// The SSID, via the wireless-extensions ioctl.
///
/// There is no wpa_cli on this device and no iwgetid, so the ioctl is the only
/// route. /proc/net/wireless reports the signal but never the name.
std::string readSsid(const char* interface) {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return std::string();
    }

    struct iwreq request;
    std::memset(&request, 0, sizeof(request));
    std::strncpy(request.ifr_name, interface, IFNAMSIZ - 1);

    char essid[IW_ESSID_MAX_SIZE + 1] = {};
    request.u.essid.pointer = essid;
    request.u.essid.length = IW_ESSID_MAX_SIZE;
    request.u.essid.flags = 0;

    std::string out;
    if (::ioctl(sock, SIOCGIWESSID, &request) == 0) {
        essid[IW_ESSID_MAX_SIZE] = '\0';
        out = essid;
    }
    ::close(sock);
    return out;
}

/// The interface this device joins networks on. Named once rather than spelled
/// at three call sites.
constexpr const char* kWirelessInterface = "wlan0";

}  // namespace

NetworkStatus Tc002Network::status() const {
    NetworkStatus result;

    // Signal strength, which was reported as a flat zero until somebody looked
    // at the tile showing it.
    const wireless::Stats signal =
        wireless::parse(readSmallFile("/proc/net/wireless"), kWirelessInterface);
    result.signalKnown = signal.known;
    result.rssiDbm = signal.levelDbm;
    result.ssid = readSsid(kWirelessInterface);

    // The lease, when something is holding one. Absent rather than zero when
    // nothing is: a device running on an address it inherited is reachable
    // right up until that address is taken back, and saying "0 seconds left"
    // would describe the opposite situation.
    if (dhcp_ != nullptr && dhcp_->running()) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const std::uint64_t millis = static_cast<std::uint64_t>(now.tv_sec) * 1000u +
                                     static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
        result.leaseKnown = true;
        result.leaseSeconds = dhcp_->remainingSeconds(millis);
        result.leaseState = net::dhcp::DhcpClient::stateName(dhcp_->state());
    }

    char hostname[128] = {};
    if (::gethostname(hostname, sizeof(hostname) - 1) == 0) {
        result.hostname = hostname;
    }

    struct ifaddrs* addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        return result;
    }

    for (struct ifaddrs* entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        const auto* in = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
        const std::uint32_t host = ntohl(in->sin_addr.s_addr);

        char text[16];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (host >> 24) & 0xFFu,
                      (host >> 16) & 0xFFu, (host >> 8) & 0xFFu, host & 0xFFu);

        result.ipv4 = text;
        result.connected = true;
        break;
    }

    ::freeifaddrs(addresses);
    return result;
}


bool Tc002Network::connected() const {
    if (control_.isOpen()) {
        return true;
    }
    // Retried rather than given up on. The supplicant is stopped while the
    // device runs its own access point, and comes back when it does not - so
    // a socket that was absent a minute ago may be there now.
    return control_.open();
}

bool Tc002Network::canScan() const { return connected(); }

bool Tc002Network::beginScan() {
    if (!connected()) {
        return false;
    }
    // "FAIL-BUSY" means a scan is already running, which is a yes from the
    // caller's point of view: results will arrive. Only a flat failure is one.
    const std::string reply = control_.ask("SCAN");
    return !reply.empty() && reply.rfind("FAIL\n", 0) != 0 &&
           reply.rfind("FAIL ", 0) != 0;
}

std::vector<WirelessNetwork> Tc002Network::networks() const {
    std::vector<WirelessNetwork> out;
    if (!connected()) {
        // The supplicant is not answering, which on this device usually means
        // the radio is busy being an access point - and that is exactly the
        // moment somebody is trying to pick a network. So the last scan is
        // served instead of an empty list, marked as remembered rather than
        // passed off as what is in range now (ADR 0013).
        live_ = false;
        return remembered_;
    }

    const std::string current = wpa::parseStatus(control_.ask("STATUS")).ssid;

    for (const wpa::Network& found : wpa::parseScanResults(control_.ask("SCAN_RESULTS"))) {
        WirelessNetwork network;
        network.ssid = found.ssid;
        network.signalDbm = found.signalDbm;
        network.secured = found.secured;
        network.current = !current.empty() && found.ssid == current;
        out.push_back(std::move(network));
    }

    // An empty answer is not an empty street.
    //
    // The guard at the top catches a socket that cannot be opened, and misses
    // the case that actually happens: the socket was opened while the station
    // was up, the hotspot then stopped wpa_supplicant, and the handle is
    // still perfectly valid-looking with nothing behind it. A live run fell
    // straight through that and served an empty list at the one moment a
    // person needed to pick a network.
    //
    // So the decision is made on what came back, not on the state of a file
    // descriptor. A scan that found something replaces the memory; a scan
    // that found nothing falls back to it and says the list is old.
    if (out.empty()) {
        live_ = false;
        return remembered_;
    }

    remembered_ = out;
    live_ = true;
    return out;
}

// --- joining -----------------------------------------------------------------

bool Tc002Network::canJoin() const {
    // Not connected(). The supplicant is deliberately stopped while the
    // hotspot is up, and that is precisely when a person is standing in front
    // of the configuration page wanting to join something. Reporting "this
    // device cannot join networks" there would be false.
    return true;
}

INetworkManager::JoinProgress Tc002Network::joinProgress() const {
    JoinProgress progress;
    progress.ssid = joinSsid_;
    progress.detail = joinDetail_;
    switch (stage_) {
        case Stage::Idle:
            progress.stage = JoinProgress::Stage::Idle;
            break;
        case Stage::Done:
            progress.stage = JoinProgress::Stage::Succeeded;
            break;
        case Stage::Failed:
            progress.stage = JoinProgress::Stage::Failed;
            break;
        default:
            progress.stage = JoinProgress::Stage::Working;
            break;
    }
    return progress;
}

void Tc002Network::fail(const std::string& why) {
    forgetAddedNetwork();
    stage_ = Stage::Failed;
    joinDetail_ = why;
    joinPassword_.clear();
}

void Tc002Network::forgetAddedNetwork() {
    if (addedNetworkId_ < 0) {
        return;
    }
    // Removed, and deliberately *not* saved.
    //
    // ADR 0018: the device appends and never replaces, and only a join that
    // produced an address is written down. Leaving the stored file untouched
    // is what makes a wrong password fall back to the network that was
    // already working rather than stranding a device nobody can reach.
    if (connected()) {
        control_.ask("REMOVE_NETWORK " + std::to_string(addedNetworkId_));
        control_.ask("RECONNECT");
    }
    addedNetworkId_ = -1;
}

bool Tc002Network::beginJoin(const std::string& ssid, const std::string& password) {
    if (stage_ != Stage::Idle && stage_ != Stage::Done && stage_ != Stage::Failed) {
        joinDetail_ = "already joining a network";
        return false;
    }

    std::string problem = wpa::ssidProblem(ssid);
    if (problem.empty() && !password.empty()) {
        problem = wpa::passphraseProblem(password);
    }
    if (!problem.empty()) {
        // Refused before anything moves, so the radio is untouched and the
        // person gets a sentence they can act on.
        stage_ = Stage::Failed;
        joinSsid_ = ssid;
        joinDetail_ = problem;
        return false;
    }

    joinSsid_ = ssid;
    joinPassword_ = password;
    joinDetail_ = "starting";
    addedNetworkId_ = -1;
    askedForAddress_ = false;
    stage_ = Stage::Settling;
    stageDeadlineMillis_ = 0;
    return true;
}

// --- remembered networks -----------------------------------------------------

bool Tc002Network::canRemember() const {
    // Only while the supplicant is running: under the hotspot it is stopped,
    // and the list is not there to read or write.
    return connected();
}

std::vector<INetworkManager::RememberedNetwork> Tc002Network::rememberedNetworks() const {
    std::vector<RememberedNetwork> out;
    if (!connected()) {
        return out;
    }
    for (const wpa::ListedNetwork& listed : wpa::parseListNetworks(control_.ask("LIST_NETWORKS"))) {
        bool seen = false;
        for (RememberedNetwork& held : out) {
            if (held.ssid == listed.ssid) {
                held.current = held.current || listed.current;
                seen = true;
            }
        }
        if (!seen) {
            out.push_back({listed.ssid, listed.current});
        }
    }
    return out;
}

bool Tc002Network::rememberNetwork(const std::string& ssid, const std::string& password,
                                   std::string& why) {
    if (!connected()) {
        why = "join a network first";
        return false;
    }
    if (stage_ != Stage::Idle && stage_ != Stage::Done && stage_ != Stage::Failed) {
        why = "already joining a network";
        return false;
    }
    std::string problem = wpa::ssidProblem(ssid);
    if (problem.empty() && !password.empty()) {
        problem = wpa::passphraseProblem(password);
    }
    if (!problem.empty()) {
        why = problem;
        return false;
    }
    const std::vector<wpa::ListedNetwork> listed =
        wpa::parseListNetworks(control_.ask("LIST_NETWORKS"));
    std::size_t others = 0;
    std::vector<int> stale;
    for (const wpa::ListedNetwork& network : listed) {
        if (network.ssid == ssid) {
            if (network.current) {
                why = "that is the network in use";
                return false;
            }
            stale.push_back(network.id);
        } else {
            ++others;
        }
    }
    if (others >= kMaxRemembered) {
        why = "five networks are remembered already - forget one first";
        return false;
    }
    std::sort(stale.begin(), stale.end(), std::greater<int>());
    for (const int old : stale) {
        control_.ask("REMOVE_NETWORK " + std::to_string(old));
    }

    const int id = wpa::parseNetworkId(control_.ask("ADD_NETWORK"));
    if (id < 0) {
        why = "this device would not accept the network";
        return false;
    }
    bool ok = wpa::succeeded(control_.ask(wpa::setNetworkHex(id, "ssid", ssid)));
    if (ok && password.empty()) {
        ok = wpa::succeeded(control_.ask(wpa::setNetworkRaw(id, "key_mgmt", "NONE")));
    } else if (ok) {
        ok = wpa::succeeded(control_.ask(wpa::setPassphrase(id, password)));
    }
    if (ok) {
        ok = wpa::succeeded(control_.ask(
            wpa::setNetworkRaw(id, "priority", std::to_string(wpa::kFallbackPriority))));
    }
    // ENABLE, not SELECT: enabling puts it on the list without leaving the
    // network in use, which is the whole point of a fallback.
    if (ok) {
        ok = wpa::succeeded(control_.ask("ENABLE_NETWORK " + std::to_string(id)));
    }
    if (!ok) {
        control_.ask("REMOVE_NETWORK " + std::to_string(id));
        why = "this device would not accept the network";
        return false;
    }
    if (!wpa::succeeded(control_.ask("SAVE_CONFIG"))) {
        why = "remembered until restart - the list could not be saved";
        return false;
    }
    return true;
}

bool Tc002Network::forgetNetwork(const std::string& ssid, std::string& why) {
    if (!connected()) {
        why = "join a network first";
        return false;
    }
    std::vector<int> ids;
    for (const wpa::ListedNetwork& network : wpa::parseListNetworks(control_.ask("LIST_NETWORKS"))) {
        if (network.ssid == ssid) {
            if (network.current) {
                why = "that is the network in use";
                return false;
            }
            ids.push_back(network.id);
        }
    }
    if (ids.empty()) {
        why = "no network by that name is remembered";
        return false;
    }
    std::sort(ids.begin(), ids.end(), std::greater<int>());
    for (const int id : ids) {
        control_.ask("REMOVE_NETWORK " + std::to_string(id));
    }
    control_.ask("SAVE_CONFIG");
    return true;
}

bool Tc002Network::configureNetwork() {
    // Replace this SSID rather than adding another copy of it.
    //
    // Without this, every trip through setup appended a block and
    // SAVE_CONFIG wrote them all. A device provisioned a few times ended up
    // with three blocks for one network, two marked `disabled=1` - seen on
    // hardware. That grows without bound, which the project rules forbid, and
    // it is one bad save away from a device that boots with every copy of its
    // network disabled and no way back except the knob.
    //
    // Removed highest-first: wpa_supplicant renumbers the ids above one that
    // goes away, so descending order keeps the rest of the list valid.
    std::vector<int> stale = wpa::networkIdsForSsid(control_.ask("LIST_NETWORKS"), joinSsid_);
    std::sort(stale.begin(), stale.end(), std::greater<int>());
    for (const int old : stale) {
        control_.ask("REMOVE_NETWORK " + std::to_string(old));
    }

    const int id = wpa::parseNetworkId(control_.ask("ADD_NETWORK"));
    if (id < 0) {
        return false;
    }
    addedNetworkId_ = id;

    if (!wpa::succeeded(control_.ask(wpa::setNetworkHex(id, "ssid", joinSsid_)))) {
        return false;
    }

    if (joinPassword_.empty()) {
        if (!wpa::succeeded(control_.ask(wpa::setNetworkRaw(id, "key_mgmt", "NONE")))) {
            return false;
        }
    } else if (!wpa::succeeded(control_.ask(wpa::setPassphrase(id, joinPassword_)))) {
        return false;
    }

    // Higher than anything stored, so the network just chosen wins without
    // the one that was working having to be removed first.
    control_.ask(wpa::setNetworkRaw(id, "priority", std::to_string(wpa::kJoinPriority)));

    if (!wpa::succeeded(control_.ask("ENABLE_NETWORK " + std::to_string(id)))) {
        return false;
    }
    control_.ask("SELECT_NETWORK " + std::to_string(id));
    return true;
}

void Tc002Network::poll(std::uint64_t nowMillis) {
    switch (stage_) {
        case Stage::Idle:
        case Stage::Done:
        case Stage::Failed:
            return;

        case Stage::Settling: {
            if (stageDeadlineMillis_ == 0) {
                // A moment before anything moves. The request that started
                // this very likely arrived over the access point it is about
                // to shut down, and the reply has to get out first.
                stageDeadlineMillis_ = nowMillis + 1500u;
                joinDetail_ = "taking the radio back";
                return;
            }
            if (nowMillis < stageDeadlineMillis_) {
                return;
            }
            if (hotspot_ != nullptr && hotspot_->running()) {
                // stop() restarts the supplicant, and the lease with it.
                hotspot_->stop();
            }
            control_.close();
            stage_ = Stage::Restoring;
            stageDeadlineMillis_ = nowMillis + 15000u;
            joinDetail_ = "waiting for Wi-Fi to come back";
            return;
        }

        case Stage::Restoring: {
            if (connected()) {
                stage_ = Stage::Configuring;
                joinDetail_ = "saving the network";
                return;
            }
            if (nowMillis >= stageDeadlineMillis_) {
                fail("the Wi-Fi service did not come back");
            }
            return;
        }

        case Stage::Configuring: {
            if (!configureNetwork()) {
                fail("this device would not accept the network");
                return;
            }
            stage_ = Stage::Associating;
            stageDeadlineMillis_ = nowMillis + 30000u;
            joinDetail_ = "connecting";
            return;
        }

        case Stage::Associating: {
            if (wpa::parseStatus(control_.ask("STATUS")).associated) {
                stage_ = Stage::Addressing;
                stageDeadlineMillis_ = nowMillis + 30000u;
                joinDetail_ = "asking for an address";
                return;
            }
            if (nowMillis >= stageDeadlineMillis_) {
                // The overwhelmingly common cause, and worth naming rather
                // than reporting a timeout nobody can act on.
                fail("could not connect - check the password");
            }
            return;
        }

        case Stage::Addressing: {
            // An association is not a connection. ADR 0018 keeps only a join
            // that produced an address, because a device associated to a
            // network it cannot be reached on is the failure that looks like
            // success.
            if (!askedForAddress_) {
                askedForAddress_ = true;
                if (dhcp_ != nullptr) {
                    // const_cast rather than carrying a second non-const
                    // pointer: this object holds the read-only view for
                    // status(), and this is the one moment it has to act.
                    const_cast<Tc002Dhcp*>(dhcp_)->restart();
                }
            }
            if (dhcp_ != nullptr && dhcp_->bound()) {
                control_.ask("SAVE_CONFIG");
                addedNetworkId_ = -1;  // kept on purpose; nothing left to undo
                stage_ = Stage::Done;
                joinDetail_ = "connected";
                joinPassword_.clear();
                return;
            }
            if (nowMillis >= stageDeadlineMillis_) {
                fail("connected, but the network gave out no address");
            }
            return;
        }
    }
}

// --- platform ---------------------------------------------------------------

bool Tc002Platform::open() {
    // Storage first: if configuration cannot persist, the host should find out
    // before it has drawn anything and started convincing the user otherwise.
    if (!storage_.open()) {
        return false;
    }
    if (!display_.open()) {
        return false;
    }
    if (!input_.open()) {
        display_.close();
        return false;
    }

    // Optional: a clock with no battery reading is still a clock, so a failure
    // here reports absence rather than refusing to start.
    mcu_.open();

    // Best effort, like the MCU. A device whose vendor audio library will
    // not load is still a clock; it just reports no speaker, and every
    // control that would have needed one disappears with it rather than
    // going quiet (ADR 0013).
    audio_.open();
    return true;
}

void Tc002Platform::close() noexcept {
    mcu_.close();
    input_.close();
    display_.close();
}

void Tc002Platform::announceRunning() const {
    const char* const argv[] = {"/bin/setprop", "sys.zkapp.state", "running", nullptr};
    runQuietly(argv);
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
