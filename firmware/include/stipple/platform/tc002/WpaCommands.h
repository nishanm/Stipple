// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stipple {
namespace platform {
namespace tc002 {
namespace wpa {

/// Building the commands that join a network.
///
/// Header-only and pure, like the replies they answer, and for a sharper
/// reason: **the SSID and the password are the two pieces of text on this
/// device that come straight from a stranger's hands into a control
/// protocol.** An SSID is whatever an access point chose to broadcast, and it
/// can contain quotes, backslashes and newlines. Building these commands with
/// string concatenation and hoping is how a network name becomes a command.
///
/// So the parts that can be escaped out of existence are, and the parts that
/// cannot are refused with a reason rather than passed through and hoped for.

/// How wpa_supplicant wants a value.
///
/// An SSID goes in as hex, always. `SET_NETWORK 0 ssid 6d796e6574` needs no
/// quoting, so there is nothing to escape and nothing to get wrong - and
/// unlike the quoted form it handles every byte an access point can legally
/// broadcast, including the ones that are not text at all.
inline std::string toHex(std::string_view value) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

/// Longest passphrase WPA allows, and the shortest it accepts.
inline constexpr std::size_t kMinPassphrase = 8;
inline constexpr std::size_t kMaxPassphrase = 63;

/// Why a passphrase was refused, or empty if it was not.
///
/// **A passphrase cannot be hex-encoded the way an SSID can.** wpa_supplicant
/// reads a bare hex value in `psk` as the 32-byte pre-shared key itself, not
/// as the passphrase to derive one from, so the quoted form is the only
/// option - and its parser takes everything up to the closing quote without
/// honouring escapes. A passphrase containing a quote therefore cannot be set
/// this way at all.
///
/// Saying so is the honest answer. Silently truncating at the quote would
/// store a different password than the one typed and report success, and the
/// device would fail to join for a reason nobody could see.
inline std::string passphraseProblem(std::string_view password) {
    if (password.size() < kMinPassphrase) {
        return "a Wi-Fi password is at least 8 characters";
    }
    if (password.size() > kMaxPassphrase) {
        return "a Wi-Fi password is at most 63 characters";
    }
    for (const char c : password) {
        if (c == '"') {
            return "this device cannot set a password containing a double quote";
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20u || byte == 0x7Fu) {
            return "the password contains a character that is not printable";
        }
    }
    return std::string();
}

/// Same, for an SSID. Hex encoding means almost nothing has to be refused.
inline std::string ssidProblem(std::string_view ssid) {
    if (ssid.empty()) {
        return "a network name is needed";
    }
    if (ssid.size() > 32) {
        return "a network name is at most 32 bytes";
    }
    return std::string();
}

/// `SET_NETWORK <id> <key> <hexValue>`
inline std::string setNetworkHex(int id, std::string_view key, std::string_view value) {
    return "SET_NETWORK " + std::to_string(id) + " " + std::string(key) + " " + toHex(value);
}

/// `SET_NETWORK <id> <key> <number>` - for priority, key_mgmt indices, and
/// anything else that is never text.
inline std::string setNetworkRaw(int id, std::string_view key, std::string_view value) {
    return "SET_NETWORK " + std::to_string(id) + " " + std::string(key) + " " +
           std::string(value);
}

/// `SET_NETWORK <id> psk "<passphrase>"`
///
/// Call `passphraseProblem` first. This does not re-check, because a caller
/// that has not checked has no message to show the person who typed it.
inline std::string setPassphrase(int id, std::string_view password) {
    return "SET_NETWORK " + std::to_string(id) + " psk \"" + std::string(password) + "\"";
}

/// What an ADD_NETWORK reply means. Returns -1 for anything that is not an id.
inline int parseNetworkId(std::string_view reply) {
    // The daemon answers with the number and a newline, and with FAIL when it
    // will not. Anything else is not something to guess at.
    int id = 0;
    std::size_t at = 0;
    while (at < reply.size() && reply[at] >= '0' && reply[at] <= '9') {
        id = id * 10 + (reply[at] - '0');
        if (id > 9999) {
            return -1;
        }
        ++at;
    }
    if (at == 0) {
        return -1;  // no digits at all: FAIL, or an error string
    }
    // Trailing whitespace is fine; trailing anything else is not.
    for (; at < reply.size(); ++at) {
        if (reply[at] != '\n' && reply[at] != '\r' && reply[at] != ' ' && reply[at] != '\0') {
            return -1;
        }
    }
    return id;
}

/// Higher than anything already stored, so a network the user just chose wins
/// without removing the one that was working.
///
/// ADR 0018 turns on this: the device appends and never replaces, so a wrong
/// password falls back to whatever was there rather than stranding a device
/// nobody can reach.
inline constexpr int kJoinPriority = 100;

/// Below any network joined on purpose, so a remembered fallback is used only
/// when the chosen one is not there.
inline constexpr int kFallbackPriority = 10;

}  // namespace wpa
}  // namespace tc002
}  // namespace platform
}  // namespace stipple
