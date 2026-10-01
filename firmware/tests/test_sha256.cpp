// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/core/Sha256.h"

#include <string>

#include "support/TestFramework.h"

using stipple::Sha256;

namespace {

std::string hex(const std::string& text) {
    return Sha256::hex(text.data(), text.size());
}

}  // namespace

STIPPLE_TEST(Sha256, MatchesKnownDigestsAcrossBlockBoundaries) {
    STIPPLE_CHECK_EQ(hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    STIPPLE_CHECK_EQ(hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    STIPPLE_CHECK_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    STIPPLE_CHECK_EQ(hex(std::string(55, 'a')), std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    STIPPLE_CHECK_EQ(hex(std::string(56, 'a')), std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    STIPPLE_CHECK_EQ(hex(std::string(64, 'a')), std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    STIPPLE_CHECK_EQ(hex(std::string(1000, 'a')), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

STIPPLE_TEST(Sha256, ALibreLinkUpUserIdHashesAsTheHeaderExpects) {
    // The account-id header is the lowercase hex digest of the user id as sent.
    STIPPLE_CHECK_EQ(hex("1e7c5ad0-0000-4000-8000-000000000000"),
                     std::string("2382bf141db0d29c12351909036047eb7ca0504a5df22a98c12e9fee11e97e7a"));
}
