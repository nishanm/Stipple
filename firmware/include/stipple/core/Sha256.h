// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace stipple {

/// SHA-256, FIPS 180-4, for the one place a glucose source needs it:
/// LibreLinkUp's `account-id` header, which is the SHA-256 of the account's
/// user id in lowercase hex. Like `Sha1`, it exists for that job and is not
/// a security primitive for anything new - TLS has its own.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;

    Sha256() noexcept { reset(); }

    void reset() noexcept;
    void update(const void* data, std::size_t length) noexcept;
    void finish(std::uint8_t out[kDigestBytes]) noexcept;

    /// Lowercase hex of `data`'s digest.
    static std::string hex(const void* data, std::size_t length);

private:
    void processBlock(const std::uint8_t block[64]) noexcept;

    std::uint32_t state_[8] = {};
    std::uint64_t bits_ = 0;
    std::uint8_t buffer_[64] = {};
    std::size_t buffered_ = 0;
};

}  // namespace stipple
