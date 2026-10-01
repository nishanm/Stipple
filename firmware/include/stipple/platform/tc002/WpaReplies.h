// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stipple {
namespace platform {
namespace tc002 {
namespace wpa {

/// Parsing for what wpa_supplicant says back.
///
/// The device has no wpa_cli binary, but wpa_supplicant runs with
/// `-C/dev/socket/` and its control socket is there as /dev/socket/wlan0. That
/// socket is the whole interface: plain text commands, plain text replies. It
/// is a far better route than editing wpa_supplicant.conf by hand, because the
/// daemon owns that file, knows how to write it, and can be told to save it.
///
/// Header-only and pure, like the MCU protocol and the rotary decoder before
/// it. A text format nobody chose is the code most likely to be wrong, and it
/// cannot be tested at all while it lives beside the socket that produced it.
///
/// **Nothing here is trusted for length.** These strings arrive from whatever
/// access points are in range, which makes an SSID the most attacker-adjacent
/// data on the device. Every field is bounded (§38).
struct Network {
    std::string ssid;
    std::string bssid;

    /// Negative dBm, closer to zero is better.
    int signalDbm = 0;
    int frequencyMhz = 0;

    /// False for an open network. Derived from the flags rather than reported
    /// raw, because "[WPA2-PSK-CCMP][ESS]" is not something to put in front of
    /// somebody choosing a network.
    bool secured = false;
};

/// What STATUS reports.
struct Status {
    bool associated = false;
    std::string ssid;
    std::string ipv4;
    /// The daemon's own word for it: COMPLETED, SCANNING, DISCONNECTED, ...
    /// Kept verbatim because a state this code does not recognise is exactly
    /// the one worth showing a person.
    std::string state;
};

/// Longest SSID the standard allows, and therefore the longest worth keeping.
inline constexpr std::size_t kMaxSsidBytes = 32;

/// Cap on how many networks one reply may produce. A busy street can see
/// dozens; a reply claiming hundreds is not a street.
inline constexpr std::size_t kMaxNetworks = 48;

namespace detail {

inline std::string clip(std::string_view text, std::size_t limit) {
    return std::string(text.substr(0, text.size() < limit ? text.size() : limit));
}

inline int toInt(std::string_view text, int fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    std::size_t at = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        at = 1;
    }
    if (at >= text.size()) {
        return fallback;
    }
    int value = 0;
    for (; at < text.size(); ++at) {
        if (text[at] < '0' || text[at] > '9') {
            return fallback;  // partly numeric is not numeric
        }
        if (value > 100000) {
            return fallback;
        }
        value = value * 10 + (text[at] - '0');
    }
    return negative ? -value : value;
}

}  // namespace detail

/// Parse a STATUS reply: `key=value` lines, in any order.
inline Status parseStatus(std::string_view reply) {
    Status status;

    std::size_t at = 0;
    while (at < reply.size()) {
        std::size_t end = reply.find('\n', at);
        if (end == std::string_view::npos) {
            end = reply.size();
        }
        const std::string_view line = reply.substr(at, end - at);
        at = end + 1;

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);

        if (key == "ssid") {
            status.ssid = detail::clip(value, kMaxSsidBytes);
        } else if (key == "ip_address") {
            status.ipv4 = detail::clip(value, 45);
        } else if (key == "wpa_state") {
            status.state = detail::clip(value, 24);
        }
    }

    // Associated means the supplicant says so, not merely that a name is
    // known: it reports the last SSID it tried while still scanning for it.
    status.associated = status.state == "COMPLETED";
    return status;
}

/// Parse a SCAN_RESULTS reply.
///
/// Tab-separated, one network per line, after a header line:
///
///     bssid / frequency / signal level / flags / ssid
///     60:a4:b7:..  2412  -42  [WPA2-PSK-CCMP][ESS]  SomeNetwork
///
/// An SSID may contain spaces, so the split is on tabs only - and the SSID is
/// the last field precisely because it is the one that can contain anything.
inline std::vector<Network> parseScanResults(std::string_view reply) {
    std::vector<Network> networks;

    std::size_t at = 0;
    bool first = true;
    while (at < reply.size() && networks.size() < kMaxNetworks) {
        std::size_t end = reply.find('\n', at);
        if (end == std::string_view::npos) {
            end = reply.size();
        }
        const std::string_view line = reply.substr(at, end - at);
        at = end + 1;

        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            // The header names its own columns, so it is recognised by content
            // rather than by being first - a reply that arrives without one
            // should not lose its first network.
            if (line.find("bssid") != std::string_view::npos &&
                line.find("ssid") != std::string_view::npos) {
                continue;
            }
        }

        std::string_view fields[5];
        int count = 0;
        std::size_t cursor = 0;
        while (count < 5 && cursor <= line.size()) {
            std::size_t tab = line.find('\t', cursor);
            if (tab == std::string_view::npos || count == 4) {
                // The last field takes the rest, so a tab inside an SSID does
                // not silently truncate it.
                fields[count++] = line.substr(cursor);
                break;
            }
            fields[count++] = line.substr(cursor, tab - cursor);
            cursor = tab + 1;
        }
        if (count < 5) {
            continue;  // not a result row
        }

        Network network;
        network.bssid = detail::clip(fields[0], 17);
        network.frequencyMhz = detail::toInt(fields[1], 0);
        network.signalDbm = detail::toInt(fields[2], 0);
        network.secured = fields[3].find("WPA") != std::string_view::npos ||
                          fields[3].find("WEP") != std::string_view::npos;
        network.ssid = detail::clip(fields[4], kMaxSsidBytes);

        // A hidden network reports an empty SSID. It cannot be joined by
        // picking it from a list, so offering it would be offering a row that
        // does nothing.
        if (network.ssid.empty()) {
            continue;
        }
        networks.push_back(std::move(network));
    }

    return networks;
}

/// Whether a reply means the command succeeded.
///
/// wpa_supplicant answers "OK", "FAIL", or the value asked for. Treating
/// anything-but-FAIL as success would read an error message as a result.
/// Network ids from a LIST_NETWORKS reply whose SSID matches `ssid`.
///
/// Exists because joining used to be pure ADD_NETWORK: every trip through
/// setup appended another block for the same network, and SAVE_CONFIG wrote
/// them all. A device re-provisioned a few times accumulated duplicates -
/// observed on hardware with three blocks for one SSID, two of them
/// `disabled=1` - which is both an unbounded growth the project rules forbid
/// and a way to boot with every copy of your network disabled.
///
/// The reply is tab-separated with a header line:
///
///     network id / ssid / bssid / flags
///     0   home    any     [CURRENT]
///     1   home    any     [DISABLED]
inline std::vector<int> networkIdsForSsid(std::string_view reply, std::string_view ssid) {
    std::vector<int> ids;
    std::size_t line = 0;
    bool first = true;
    while (line < reply.size()) {
        std::size_t end = reply.find('\n', line);
        if (end == std::string_view::npos) {
            end = reply.size();
        }
        const std::string_view row = reply.substr(line, end - line);
        line = end + 1;

        // The header names the columns rather than describing a network.
        if (first) {
            first = false;
            continue;
        }
        if (row.empty()) {
            continue;
        }

        const std::size_t firstTab = row.find('\t');
        if (firstTab == std::string_view::npos) {
            continue;
        }
        const std::size_t secondTab = row.find('\t', firstTab + 1);
        const std::string_view name =
            row.substr(firstTab + 1, secondTab == std::string_view::npos
                                         ? std::string_view::npos
                                         : secondTab - firstTab - 1);
        if (name != ssid) {
            continue;
        }
        const int id = detail::toInt(row.substr(0, firstTab), -1);
        if (id >= 0) {
            ids.push_back(id);
        }
    }
    return ids;
}

/// Every network block in a LIST_NETWORKS reply: its id, SSID, and whether it
/// is the one in use ([CURRENT]).
struct ListedNetwork {
    int id = -1;
    std::string ssid;
    bool current = false;
};

inline std::vector<ListedNetwork> parseListNetworks(std::string_view reply) {
    std::vector<ListedNetwork> out;
    std::size_t line = 0;
    bool first = true;
    while (line < reply.size()) {
        std::size_t end = reply.find('\n', line);
        if (end == std::string_view::npos) {
            end = reply.size();
        }
        const std::string_view row = reply.substr(line, end - line);
        line = end + 1;
        if (first) {
            first = false;
            continue;
        }
        const std::size_t firstTab = row.find('\t');
        if (row.empty() || firstTab == std::string_view::npos) {
            continue;
        }
        const std::size_t secondTab = row.find('\t', firstTab + 1);
        ListedNetwork network;
        network.id = detail::toInt(row.substr(0, firstTab), -1);
        network.ssid = std::string(row.substr(
            firstTab + 1,
            secondTab == std::string_view::npos ? std::string_view::npos : secondTab - firstTab - 1));
        network.current = row.find("[CURRENT]") != std::string_view::npos;
        if (network.id >= 0) {
            out.push_back(std::move(network));
        }
    }
    return out;
}

inline bool succeeded(std::string_view reply) noexcept {
    return reply.rfind("OK", 0) == 0;
}

}  // namespace wpa
}  // namespace tc002
}  // namespace platform
}  // namespace stipple
