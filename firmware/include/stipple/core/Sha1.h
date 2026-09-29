// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace stipple {

/// SHA-1, because Nightscout authenticates with it and we do not get to choose.
///
/// **Not a security primitive for anything new.** SHA-1 has had practical
/// collisions since 2017. It is here for exactly one job: Nightscout's REST API
/// accepts an `api-secret` header carrying the SHA-1 of the site's secret, so
/// a device that reads glucose from Nightscout has to be able to produce one.
/// Storing that digest rather than the secret it came from is the small win
/// this buys - the plaintext never has to live on the device.
///
/// Incremental like Md5, and written from FIPS 180-4 rather than borrowed:
/// BearSSL ships one, but it is fenced inside the device adapter on purpose and
/// the simulator and the host tests need this too.
class Sha1 {
public:
    static constexpr std::size_t kDigestBytes = 20;

    Sha1() noexcept { reset(); }

    void reset() noexcept;
    void update(const void* data, std::size_t length) noexcept;

    /// Finish and write the digest. The object must not be updated again
    /// without `reset()`.
    void finish(std::uint8_t out[kDigestBytes]) noexcept;

    /// Lowercase hex, the form the header wants.
    std::string finishHex();

    /// One-shot, for the common case.
    static std::string hex(const void* data, std::size_t length);

private:
    void processBlock(const std::uint8_t block[64]) noexcept;

    std::uint32_t state_[5] = {};
    std::uint64_t bits_ = 0;
    std::uint8_t buffer_[64] = {};
    std::size_t buffered_ = 0;
    bool finished_ = false;
};

}  // namespace stipple
