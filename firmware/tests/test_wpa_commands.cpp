// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/WpaCommands.h"

#include <string>

#include "support/TestFramework.h"

using stipple::platform::tc002::wpa::kJoinPriority;
using stipple::platform::tc002::wpa::parseNetworkId;
using stipple::platform::tc002::wpa::passphraseProblem;
using stipple::platform::tc002::wpa::setNetworkHex;
using stipple::platform::tc002::wpa::setPassphrase;
using stipple::platform::tc002::wpa::ssidProblem;
using stipple::platform::tc002::wpa::toHex;

STIPPLE_TEST(WpaCommands, HexEncodesAnSsid) {
    STIPPLE_CHECK_EQ(toHex("abc"), std::string("616263"));
    STIPPLE_CHECK_EQ(toHex(""), std::string());
    // High bytes, which an SSID is allowed to contain and which the quoted
    // form would mangle.
    STIPPLE_CHECK_EQ(toHex(std::string("\x00\xff", 2)), std::string("00ff"));
}

STIPPLE_TEST(WpaCommands, AnSsidCannotEscapeIntoACommand) {
    // The point of hex. This name is a deliberate attempt to end the command
    // and start another one, and it comes out as data either way.
    const std::string hostile = "evil\" \nLIST_NETWORKS\n";
    const std::string command = setNetworkHex(0, "ssid", hostile);

    STIPPLE_CHECK(command.find('"') == std::string::npos);
    STIPPLE_CHECK(command.find('\n') == std::string::npos);
    STIPPLE_CHECK(command.find("LIST_NETWORKS") == std::string::npos);
    STIPPLE_CHECK_EQ(command, "SET_NETWORK 0 ssid " + toHex(hostile));
}

STIPPLE_TEST(WpaCommands, BuildsTheCommandsWpaSupplicantExpects) {
    STIPPLE_CHECK_EQ(setNetworkHex(3, "ssid", "home"), std::string("SET_NETWORK 3 ssid 686f6d65"));
    STIPPLE_CHECK_EQ(setPassphrase(3, "hunter22"),
                    std::string("SET_NETWORK 3 psk \"hunter22\""));
}

STIPPLE_TEST(WpaCommands, RefusesAPassphraseItCannotSetHonestly) {
    // A passphrase cannot be hex-encoded - wpa_supplicant would read the hex
    // as the derived key rather than the words - so the quoted form is the
    // only one, and its parser stops at the first quote without honouring
    // escapes. Truncating there would store a different password than the one
    // typed and then report success.
    const std::string problem = passphraseProblem("has\"quote");
    STIPPLE_CHECK(!problem.empty());
    STIPPLE_CHECK(problem.find("double quote") != std::string::npos);
}

STIPPLE_TEST(WpaCommands, RefusesPassphrasesThatCannotWork) {
    // The two lengths either side of WPA's 63-character ceiling, named rather
    // than built inside the call.
    //
    // Naming them also sidesteps a GCC 13 false positive. At -O3 it cannot
    // tell a std::string temporary's small-string buffer from a heap pointer
    // and reports -Wfree-nonheap-object on the destructor, which with -Werror
    // failed the Linux build while MSVC said nothing at all. The code was
    // correct both times: passphraseProblem takes a string_view and the
    // temporary outlives the call.
    const std::string tooLong(64, 'a');
    const std::string longestAllowed(63, 'a');

    STIPPLE_CHECK(!passphraseProblem("short").empty());   // under 8
    STIPPLE_CHECK(!passphraseProblem(tooLong).empty());   // over 63
    STIPPLE_CHECK(!passphraseProblem("line\nbreak").empty());
    STIPPLE_CHECK(!passphraseProblem(std::string("nul\0byte", 8)).empty());

    // And accepts the ones that can. Backslashes are fine: the quoted form
    // does not treat them as escapes, so they arrive as typed.
    STIPPLE_CHECK(passphraseProblem("hunter22").empty());
    STIPPLE_CHECK(passphraseProblem("back\\slash").empty());
    STIPPLE_CHECK(passphraseProblem(longestAllowed).empty());
    STIPPLE_CHECK(passphraseProblem("~!@#$%^&*()_+{}|:<>?").empty());
}

STIPPLE_TEST(WpaCommands, ChecksAnSsidIsOneAtAll) {
    STIPPLE_CHECK(!ssidProblem("").empty());
    STIPPLE_CHECK(!ssidProblem(std::string(33, 'a')).empty());
    STIPPLE_CHECK(ssidProblem("a").empty());
    STIPPLE_CHECK(ssidProblem(std::string(32, 'a')).empty());
    // Hostile names are allowed through - hex encoding is what makes them
    // safe, not refusing them.
    STIPPLE_CHECK(ssidProblem("evil\" \nSTUFF").empty());
}

STIPPLE_TEST(WpaCommands, ReadsAnAddNetworkReply) {
    STIPPLE_CHECK_EQ(parseNetworkId("0\n"), 0);
    STIPPLE_CHECK_EQ(parseNetworkId("12"), 12);
    STIPPLE_CHECK_EQ(parseNetworkId("3\r\n"), 3);
}

STIPPLE_TEST(WpaCommands, RefusesAnAddNetworkReplyThatIsNotAnId) {
    // FAIL is the common one, and treating it as network 0 would mean
    // reconfiguring whichever network happened to be first - most likely the
    // one currently working.
    STIPPLE_CHECK_EQ(parseNetworkId("FAIL\n"), -1);
    STIPPLE_CHECK_EQ(parseNetworkId(""), -1);
    STIPPLE_CHECK_EQ(parseNetworkId("UNKNOWN COMMAND"), -1);
    STIPPLE_CHECK_EQ(parseNetworkId("0 extra"), -1);
    STIPPLE_CHECK_EQ(parseNetworkId("99999999"), -1);
}

STIPPLE_TEST(WpaCommands, JoinsAtAHigherPriorityThanWhatIsStored) {
    // ADR 0018 turns on this: the device appends and never replaces, so the
    // network just chosen has to outrank the ones already there - and a wrong
    // password falls back to whatever was working instead of stranding a
    // device nobody can reach.
    STIPPLE_CHECK(kJoinPriority > 0);
}
