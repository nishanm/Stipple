// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/core/Sha1.h"

#include <cstring>

namespace stipple {
namespace {

constexpr std::uint32_t rotateLeft(std::uint32_t value, unsigned bits) noexcept {
    return (value << bits) | (value >> (32u - bits));
}

}  // namespace

void Sha1::reset() noexcept {
    state_[0] = 0x67452301u;
    state_[1] = 0xEFCDAB89u;
    state_[2] = 0x98BADCFEu;
    state_[3] = 0x10325476u;
    state_[4] = 0xC3D2E1F0u;
    bits_ = 0;
    buffered_ = 0;
    finished_ = false;
}

void Sha1::processBlock(const std::uint8_t block[64]) noexcept {
    std::uint32_t w[80];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];

    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f;
        std::uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotateLeft(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const void* data, std::size_t length) noexcept {
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    bits_ += static_cast<std::uint64_t>(length) * 8u;

    if (buffered_ > 0) {
        const std::size_t room = 64 - buffered_;
        const std::size_t take = length < room ? length : room;
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        length -= take;
        if (buffered_ < 64) {
            return;
        }
        processBlock(buffer_);
        buffered_ = 0;
    }

    while (length >= 64) {
        processBlock(bytes);
        bytes += 64;
        length -= 64;
    }

    if (length > 0) {
        std::memcpy(buffer_, bytes, length);
        buffered_ = length;
    }
}

void Sha1::finish(std::uint8_t out[kDigestBytes]) noexcept {
    if (!finished_) {
        const std::uint64_t bits = bits_;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (buffered_ != 56) {
            update(&zero, 1);
        }
        std::uint8_t length[8];
        for (std::size_t i = 0; i < 8; ++i) {
            length[i] = static_cast<std::uint8_t>(bits >> (56u - 8u * i));
        }
        update(length, 8);
        finished_ = true;
    }
    for (std::size_t i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
}

std::string Sha1::finishHex() {
    std::uint8_t digest[kDigestBytes];
    finish(digest);
    static const char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(kDigestBytes * 2);
    for (const std::uint8_t byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0Fu]);
    }
    return text;
}

std::string Sha1::hex(const void* data, std::size_t length) {
    Sha1 hash;
    hash.update(data, length);
    return hash.finishHex();
}

}  // namespace stipple
