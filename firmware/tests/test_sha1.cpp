// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/core/Sha1.h"

#include <string>

#include "support/TestFramework.h"

using stipple::Sha1;

namespace {

std::string hex(const std::string& text) {
    return Sha1::hex(text.data(), text.size());
}

}  // namespace

STIPPLE_TEST(Sha1, MatchesTheVectorsInFips180) {
    STIPPLE_CHECK_EQ(hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    STIPPLE_CHECK_EQ(hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    STIPPLE_CHECK_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                     std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    STIPPLE_CHECK_EQ(hex("The quick brown fox jumps over the lazy dog"),
                     std::string("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"));
}

STIPPLE_TEST(Sha1, HandlesTheBlockBoundariesThatTripImplementationsUp) {
    // 55, 56 and 64 bytes are where the padding decides whether it needs one
    // extra block.
    STIPPLE_CHECK_EQ(hex(std::string(55, 'a')),
                     std::string("c1c8bbdc22796e28c0e15163d20899b65621d65a"));
    STIPPLE_CHECK_EQ(hex(std::string(56, 'a')),
                     std::string("c2db330f6083854c99d4b5bfb6e8f29f201be699"));
    STIPPLE_CHECK_EQ(hex(std::string(64, 'a')),
                     std::string("0098ba824b5c16427bd7a1122a5a442a25ec644d"));
}

STIPPLE_TEST(Sha1, IncrementalUpdatesMatchOneShot) {
    const std::string text = "The quick brown fox jumps over the lazy dog";
    Sha1 hash;
    for (const char c : text) {
        hash.update(&c, 1);
    }
    STIPPLE_CHECK_EQ(hash.finishHex(), hex(text));
}

STIPPLE_TEST(Sha1, AMillionAsInPieces) {
    // The FIPS long vector, fed in odd-sized pieces so every buffer path runs.
    Sha1 hash;
    const std::string piece(997, 'a');
    std::size_t sent = 0;
    while (sent + piece.size() <= 1000000) {
        hash.update(piece.data(), piece.size());
        sent += piece.size();
    }
    const std::string rest(1000000 - sent, 'a');
    hash.update(rest.data(), rest.size());
    STIPPLE_CHECK_EQ(hash.finishHex(), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}
