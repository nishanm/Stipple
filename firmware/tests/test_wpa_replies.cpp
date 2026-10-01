// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/WpaReplies.h"

#include <string>

#include "support/TestFramework.h"

using stipple::platform::tc002::wpa::kMaxNetworks;
using stipple::platform::tc002::wpa::kMaxSsidBytes;
using stipple::platform::tc002::wpa::Network;
using stipple::platform::tc002::wpa::parseScanResults;
using stipple::platform::tc002::wpa::parseStatus;
using stipple::platform::tc002::wpa::Status;
using stipple::platform::tc002::wpa::succeeded;

namespace {

/// Shaped exactly like the reply read off the device, with the identifying
/// details replaced - a test fixture should not carry somebody's network name
/// or MAC into a public repository.
constexpr const char* kStatus =
    "bssid=00:11:22:33:44:55\n"
    "freq=2412\n"
    "ssid=Example Network\n"
    "id=0\n"
    "mode=station\n"
    "pairwise_cipher=CCMP\n"
    "group_cipher=CCMP\n"
    "key_mgmt=WPA2-PSK\n"
    "wpa_state=COMPLETED\n"
    "ip_address=192.168.1.238\n"
    "address=aa:bb:cc:dd:ee:ff\n"
    "uuid=00000000-0000-0000-0000-000000000000\n";

constexpr const char* kScan =
    "bssid / frequency / signal level / flags / ssid\n"
    "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tExample Network\n"
    "00:11:22:33:44:66\t5180\t-71\t[WPA2-PSK-CCMP][WPS][ESS]\tExample 5G\n"
    "00:11:22:33:44:77\t2437\t-88\t[ESS]\tGuest Open\n";

}  // namespace

STIPPLE_TEST(WpaReplies, ReadsAStatusReply) {
    const Status status = parseStatus(kStatus);
    STIPPLE_CHECK(status.associated);
    STIPPLE_CHECK_EQ(status.ssid, std::string("Example Network"));
    STIPPLE_CHECK_EQ(status.ipv4, std::string("192.168.1.238"));
    STIPPLE_CHECK_EQ(status.state, std::string("COMPLETED"));
}

STIPPLE_TEST(WpaReplies, AssociatedMeansTheDaemonSaysSoNotThatANameIsKnown) {
    // The supplicant reports the SSID it is trying while still scanning for
    // it. Treating a known name as a connection would show a device as online
    // while it is hunting for a network that may not be there.
    const Status hunting = parseStatus(
        "ssid=Example Network\n"
        "wpa_state=SCANNING\n");
    STIPPLE_CHECK_FALSE(hunting.associated);
    STIPPLE_CHECK_EQ(hunting.ssid, std::string("Example Network"));
    STIPPLE_CHECK_EQ(hunting.state, std::string("SCANNING"));
}

STIPPLE_TEST(WpaReplies, AnUnknownStateIsKeptVerbatim) {
    // A state this code does not recognise is exactly the one worth showing a
    // person, rather than flattening to "disconnected".
    const Status odd = parseStatus("wpa_state=INTERFACE_DISABLED\n");
    STIPPLE_CHECK_EQ(odd.state, std::string("INTERFACE_DISABLED"));
    STIPPLE_CHECK_FALSE(odd.associated);
}

STIPPLE_TEST(WpaReplies, ReadsScanResults) {
    const std::vector<Network> networks = parseScanResults(kScan);
    STIPPLE_REQUIRE(networks.size() == 3);

    STIPPLE_CHECK_EQ(networks[0].ssid, std::string("Example Network"));
    STIPPLE_CHECK_EQ(networks[0].signalDbm, -42);
    STIPPLE_CHECK_EQ(networks[0].frequencyMhz, 2412);
    STIPPLE_CHECK(networks[0].secured);

    STIPPLE_CHECK_EQ(networks[1].signalDbm, -71);
    STIPPLE_CHECK_EQ(networks[1].frequencyMhz, 5180);

    // An open network, which has to be distinguishable or somebody is asked
    // for a password that does not exist.
    STIPPLE_CHECK_EQ(networks[2].ssid, std::string("Guest Open"));
    STIPPLE_CHECK_FALSE(networks[2].secured);
}

STIPPLE_TEST(WpaReplies, TheHeaderIsRecognisedByContentNotByPosition) {
    // A reply that arrives without a header should not lose its first network.
    const std::vector<Network> headerless = parseScanResults(
        "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tOnly One\n");
    STIPPLE_REQUIRE(headerless.size() == 1);
    STIPPLE_CHECK_EQ(headerless[0].ssid, std::string("Only One"));
}

STIPPLE_TEST(WpaReplies, AnSsidMayContainSpacesAndEvenATab) {
    // The SSID is the last field precisely because it is the one that can
    // contain anything. Splitting it on whitespace loses half of somebody's
    // network name and they never find out why it will not connect.
    const std::vector<Network> networks = parseScanResults(
        "00:11:22:33:44:55\t2412\t-42\t[ESS]\tThe Smiths Wi-Fi 2.4\n");
    STIPPLE_REQUIRE(networks.size() == 1);
    STIPPLE_CHECK_EQ(networks[0].ssid, std::string("The Smiths Wi-Fi 2.4"));
}

STIPPLE_TEST(WpaReplies, HiddenNetworksAreLeftOut) {
    // An empty SSID cannot be joined by picking it from a list, so a row for
    // it would be a row that does nothing.
    const std::vector<Network> networks = parseScanResults(
        "bssid / frequency / signal level / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\t\n"
        "00:11:22:33:44:66\t2412\t-50\t[ESS]\tVisible\n");
    STIPPLE_REQUIRE(networks.size() == 1);
    STIPPLE_CHECK_EQ(networks[0].ssid, std::string("Visible"));
}

STIPPLE_TEST(WpaReplies, AnSsidIsBoundedBecauseItComesFromStrangers) {
    // The most attacker-adjacent data on the device: anybody within radio
    // range chooses it. §38 says bounded, and 32 bytes is what the standard
    // allows anyway.
    std::string line = "00:11:22:33:44:55\t2412\t-42\t[ESS]\t";
    line += std::string(500, 'x');
    line += "\n";

    const std::vector<Network> networks = parseScanResults(line);
    STIPPLE_REQUIRE(networks.size() == 1);
    STIPPLE_CHECK_EQ(networks[0].ssid.size(), kMaxSsidBytes);
}

STIPPLE_TEST(WpaReplies, TheNumberOfNetworksIsBounded) {
    // A busy street sees dozens. A reply claiming hundreds is not a street,
    // and this runs on a device with 36 MB.
    std::string reply = "bssid / frequency / signal level / flags / ssid\n";
    for (int i = 0; i < 400; ++i) {
        reply += "00:11:22:33:44:55\t2412\t-42\t[ESS]\tNet" + std::to_string(i) + "\n";
    }
    STIPPLE_CHECK(parseScanResults(reply).size() <= kMaxNetworks);
}

STIPPLE_TEST(WpaReplies, RubbishProducesNothingRatherThanHalfARow) {
    STIPPLE_CHECK(parseScanResults("").empty());
    STIPPLE_CHECK(parseScanResults("FAIL\n").empty());
    STIPPLE_CHECK(parseScanResults("not\ttab\tseparated\n").empty());
    STIPPLE_CHECK(parseScanResults("bssid / frequency / signal level / flags / ssid\n").empty());

    const Status nothing = parseStatus("");
    STIPPLE_CHECK_FALSE(nothing.associated);
    STIPPLE_CHECK(nothing.ssid.empty());
}

STIPPLE_TEST(WpaReplies, NonNumericFieldsFallBackRatherThanBeingHalfRead) {
    const std::vector<Network> networks = parseScanResults(
        "00:11:22:33:44:55\tnonsense\t-42dB\t[ESS]\tOdd\n");
    STIPPLE_REQUIRE(networks.size() == 1);
    STIPPLE_CHECK_EQ(networks[0].frequencyMhz, 0);
    // "-42dB" is partly numeric, which is not numeric - reading it as -42
    // would be inventing confidence the reply does not support.
    STIPPLE_CHECK_EQ(networks[0].signalDbm, 0);
}

STIPPLE_TEST(WpaReplies, OnlyOkMeansSuccess) {
    // wpa_supplicant answers OK, FAIL, or the value asked for. Treating
    // anything-but-FAIL as success would read an error message as a result.
    STIPPLE_CHECK(succeeded("OK\n"));
    STIPPLE_CHECK_FALSE(succeeded("FAIL\n"));
    STIPPLE_CHECK_FALSE(succeeded(""));
    STIPPLE_CHECK_FALSE(succeeded("UNKNOWN COMMAND\n"));
    STIPPLE_CHECK_FALSE(succeeded("0\n"));
}

STIPPLE_TEST(WpaReplies, FindsEveryCopyOfAnSsid) {
    // Three blocks for one network is not hypothetical - it is what a device
    // looked like after being provisioned a few times, and two of them were
    // disabled.
    const std::string reply =
        "network id / ssid / bssid / flags\n"
        "0\thome\tany\t[DISABLED]\n"
        "1\tother\tany\t\n"
        "2\thome\tany\t[DISABLED]\n"
        "3\thome\tany\t[CURRENT]\n";
    const std::vector<int> ids = stipple::platform::tc002::wpa::networkIdsForSsid(reply, "home");
    STIPPLE_CHECK_EQ(ids.size(), std::size_t{3});
    STIPPLE_CHECK_EQ(ids[0], 0);
    STIPPLE_CHECK_EQ(ids[1], 2);
    STIPPLE_CHECK_EQ(ids[2], 3);
}

STIPPLE_TEST(WpaReplies, DoesNotMatchOtherNetworks) {
    const std::string reply =
        "network id / ssid / bssid / flags\n"
        "0\thome\tany\t\n"
        "1\thome-guest\tany\t\n";
    // Prefix matching here would delete the neighbour's network, not ours.
    const std::vector<int> ids = stipple::platform::tc002::wpa::networkIdsForSsid(reply, "home");
    STIPPLE_CHECK_EQ(ids.size(), std::size_t{1});
    STIPPLE_CHECK_EQ(ids[0], 0);
}

STIPPLE_TEST(WpaReplies, SurvivesAnEmptyOrHeaderOnlyList) {
    using stipple::platform::tc002::wpa::networkIdsForSsid;
    STIPPLE_CHECK(networkIdsForSsid("", "home").empty());
    STIPPLE_CHECK(networkIdsForSsid("network id / ssid / bssid / flags\n", "home").empty());
    STIPPLE_CHECK(networkIdsForSsid("FAIL\n", "home").empty());
}

STIPPLE_TEST(WpaReplies, ListNetworksNamesEachBlockAndTheOneInUse) {
    const std::string reply =
        "network id / ssid / bssid / flags\n"
        "0\tHome\tany\t[CURRENT]\n"
        "1\tGrandma\tany\t\n"
        "2\tHome\tany\t[DISABLED]\n";
    const auto listed = stipple::platform::tc002::wpa::parseListNetworks(reply);
    STIPPLE_REQUIRE(listed.size() == 3);
    STIPPLE_CHECK_EQ(listed[0].ssid, std::string("Home"));
    STIPPLE_CHECK(listed[0].current);
    STIPPLE_CHECK_EQ(listed[1].id, 1);
    STIPPLE_CHECK_EQ(listed[1].ssid, std::string("Grandma"));
    STIPPLE_CHECK_FALSE(listed[1].current);
}
