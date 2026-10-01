// SPDX-License-Identifier: GPL-3.0-or-later
//
// `# @config` — settings a script declares for the web UI to collect.
//
// The point of the feature is that nobody edits Berry to change a channel ID.
// The point of these tests is that a typo in a declaration costs that field
// and nothing else: the source still compiles, the app still runs, and the
// editor shows whatever was understood. A settings parser that can fail a
// script is worse than no settings parser.
#include "stipple/script/ScriptConfig.h"

#include <string>

#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/script/ScriptStore.h"
#include "support/TestFramework.h"

using stipple::Canvas;
using stipple::Framebuffer;
using stipple::script::parseSettings;
using stipple::script::ScriptPutResult;
using stipple::script::ScriptStore;
using stipple::script::Setting;

STIPPLE_TEST(ScriptConfig, TheDeclarationFromTheRequestParses) {
    // Verbatim from the line this feature was asked for, including the help
    // text with a comma and an @ in it - both of which a naive split would
    // have eaten.
    const auto settings = parseSettings(
        "# @config  chan  text   \"Channel ID\" default=\"UCpGLALzRO0uaasWTsm9M99w\" "
        "maxlen=32 help=\"The UC... part of youtube.com/channel/UC..., not the @handle\"\n"
        "\n"
        "class App\n");

    STIPPLE_REQUIRE(settings.size() == 1);
    const Setting& one = settings[0];
    STIPPLE_CHECK(one.key == "chan");
    STIPPLE_CHECK(one.type == Setting::Type::Text);
    STIPPLE_CHECK(one.label == "Channel ID");
    STIPPLE_CHECK(one.fallback == "UCpGLALzRO0uaasWTsm9M99w");
    STIPPLE_CHECK_EQ(one.maxLength, 32);
    STIPPLE_CHECK(one.help.find("not the @handle") != std::string::npos);
}

STIPPLE_TEST(ScriptConfig, EveryTypeIsUnderstood) {
    const auto settings = parseSettings(
        "# @config name   text    \"Name\"\n"
        "# @config count  number  \"How many\" min=1 max=10 default=3\n"
        "# @config loud   boolean \"Make a noise\" default=true\n");

    STIPPLE_REQUIRE(settings.size() == 3);
    STIPPLE_CHECK(settings[0].type == Setting::Type::Text);
    STIPPLE_CHECK(settings[1].type == Setting::Type::Number);
    STIPPLE_CHECK(settings[2].type == Setting::Type::Boolean);

    STIPPLE_CHECK(settings[1].bounded);
    STIPPLE_CHECK_EQ(static_cast<int>(settings[1].minimum), 1);
    STIPPLE_CHECK_EQ(static_cast<int>(settings[1].maximum), 10);
}

STIPPLE_TEST(ScriptConfig, TheHeaderEndsAtTheFirstRealLine) {
    // A `@config` written halfway down is a comment about the code beside it,
    // and honouring it would be a guess. Same rule as the shop's header.
    const auto settings = parseSettings(
        "# @config top text \"Counted\"\n"
        "import string\n"
        "# @config buried text \"Not counted\"\n");

    STIPPLE_REQUIRE(settings.size() == 1);
    STIPPLE_CHECK(settings[0].key == "top");
}

STIPPLE_TEST(ScriptConfig, BlankLinesInTheHeaderDoNotEndIt) {
    const auto settings = parseSettings(
        "# name: Something\n"
        "\n"
        "# @config chan text \"Channel\"\n");
    STIPPLE_REQUIRE(settings.size() == 1);
}

STIPPLE_TEST(ScriptConfig, AMalformedDeclarationCostsOnlyItself) {
    // The whole reason the parser skips rather than fails. A script with a
    // typo'd settings line should still run.
    const auto settings = parseSettings(
        "# @config\n"                                  // nothing at all
        "# @config UPPER text \"Bad key\"\n"           // keys are lowercase
        "# @config ok1 colour \"Unknown type\"\n"      // no such type
        "# @config ok2 text\n"                         // no label
        "# @config good text \"Fine\"\n");

    STIPPLE_REQUIRE(settings.size() == 1);
    STIPPLE_CHECK(settings[0].key == "good");
}

STIPPLE_TEST(ScriptConfig, TheNumberOfSettingsIsBounded) {
    std::string source;
    for (int i = 0; i < 40; ++i) {
        source += "# @config k" + std::to_string(i) + " text \"Field\"\n";
    }
    const auto settings = parseSettings(source);
    STIPPLE_CHECK(settings.size() == stipple::script::kMaxSettings);
}

STIPPLE_TEST(ScriptConfig, ADuplicateKeyKeepsTheFirst) {
    // So the result does not depend on how far down the file the reader got.
    const auto settings = parseSettings(
        "# @config chan text \"First\"\n"
        "# @config chan text \"Second\"\n");
    STIPPLE_REQUIRE(settings.size() == 1);
    STIPPLE_CHECK(settings[0].label == "First");
}

STIPPLE_TEST(ScriptConfig, AScriptWithNoDeclarationsHasNone) {
    STIPPLE_CHECK(parseSettings("class App\n  def draw()\n  end\nend\n").empty());
    STIPPLE_CHECK(parseSettings("").empty());
    STIPPLE_CHECK(parseSettings("# just a comment\n").empty());
}

STIPPLE_TEST(ScriptConfig, AnUnclosedQuoteTakesTheRestOfTheLine) {
    // Rather than discarding the declaration over one missing character at
    // the end, when what the author meant is obvious.
    const auto settings = parseSettings("# @config chan text \"Channel ID\n");
    STIPPLE_REQUIRE(settings.size() == 1);
    STIPPLE_CHECK(settings[0].label == "Channel ID");
}

// --- through the store -------------------------------------------------------

STIPPLE_TEST(ScriptSettings, AValueSetFromOutsideIsWhatTheScriptReads) {
    // The whole feature in one test: the web UI writes, and the script picks
    // it up through the `store.get` it was already using.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("yt", "YouTube", R"BE(
# @config chan text "Channel ID" default="UCdefault"
class App
  def draw()
    var id = store.get("chan", "UCdefault")
    if id == "UCchosen"
      pixel(0, 0, rgb(0, 255, 0))
    end
  end
end
return App()
)BE") == ScriptPutResult::Added);

    const auto settings = store.settings("yt");
    STIPPLE_REQUIRE(settings.size() == 1);
    STIPPLE_CHECK(settings[0].fallback == "UCdefault");
    // Nothing set yet, which is not the same as set to empty.
    STIPPLE_CHECK(store.settingValue("yt", "chan").empty());

    STIPPLE_REQUIRE(store.setSetting("yt", "chan", "UCchosen"));
    STIPPLE_CHECK(store.settingValue("yt", "chan") == "UCchosen");

    Framebuffer framebuffer;
    Canvas canvas(framebuffer);
    STIPPLE_REQUIRE(store.draw("yt", canvas, 0));
    STIPPLE_CHECK(framebuffer.at(0, 0) != stipple::colors::kBlack);
}

STIPPLE_TEST(ScriptSettings, TypesSurviveTheRoundTrip) {
    // A boolean has to come back as a Berry boolean, not the string "true",
    // or `store.get("loud", false)` compares against something that is
    // always truthy.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("t", "Types", R"BE(
# @config loud boolean "Noise" default=false
# @config many number "Count" min=0 max=9 default=1
class App
  def draw()
    if store.get("loud", false) == true
      pixel(0, 0, rgb(0, 255, 0))
    end
    if store.get("many", 0) == 7
      pixel(1, 0, rgb(0, 255, 0))
    end
  end
end
return App()
)BE") == ScriptPutResult::Added);

    STIPPLE_REQUIRE(store.setSetting("t", "loud", "true"));
    STIPPLE_REQUIRE(store.setSetting("t", "many", "7"));

    Framebuffer framebuffer;
    Canvas canvas(framebuffer);
    STIPPLE_REQUIRE(store.draw("t", canvas, 0));
    STIPPLE_CHECK(framebuffer.at(0, 0) != stipple::colors::kBlack);
    STIPPLE_CHECK(framebuffer.at(1, 0) != stipple::colors::kBlack);
}

STIPPLE_TEST(ScriptSettings, OnlyDeclaredKeysCanBeWritten) {
    // Otherwise the API would be a way to write arbitrary entries into a
    // script's private store from outside, which is a different feature with
    // different consequences.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("s", "S", R"BE(
# @config chan text "Channel"
class App
  def draw()
  end
end
return App()
)BE") == ScriptPutResult::Added);

    STIPPLE_CHECK(store.setSetting("s", "chan", "ok"));
    STIPPLE_CHECK(!store.setSetting("s", "secret", "no"));
    STIPPLE_CHECK(!store.setSetting("nosuchscript", "chan", "no"));
}

STIPPLE_TEST(ScriptSettings, AValueThatDoesNotFitIsRefused) {
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("s", "S", R"BE(
# @config code text "Code" maxlen=4
# @config many number "Count" min=1 max=10
# @config loud boolean "Noise"
class App
  def draw()
  end
end
return App()
)BE") == ScriptPutResult::Added);

    STIPPLE_CHECK(store.setSetting("s", "code", "abcd"));
    STIPPLE_CHECK(!store.setSetting("s", "code", "abcde"));      // over maxlen

    STIPPLE_CHECK(store.setSetting("s", "many", "10"));
    STIPPLE_CHECK(!store.setSetting("s", "many", "11"));         // over max
    STIPPLE_CHECK(!store.setSetting("s", "many", "0"));          // under min
    STIPPLE_CHECK(!store.setSetting("s", "many", "banana"));     // not a number

    STIPPLE_CHECK(store.setSetting("s", "loud", "on"));          // what a form sends
    STIPPLE_CHECK(!store.setSetting("s", "loud", "maybe"));
}

STIPPLE_TEST(ScriptSettings, SettingOneMarksTheLibraryForSaving) {
    // A setting that survived until the next reboot and then reverted would
    // be the worst of both: it appears to work.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("s", "S", R"BE(
# @config chan text "Channel"
class App
  def draw()
  end
end
return App()
)BE") == ScriptPutResult::Added);

    const std::uint32_t before = store.revision();
    STIPPLE_REQUIRE(store.setSetting("s", "chan", "value"));
    STIPPLE_CHECK(store.revision() != before);

    // And it is in the blob, so it comes back.
    const std::string blob = store.serialize();
    ScriptStore restored;
    STIPPLE_REQUIRE(restored.deserialize(blob));
    STIPPLE_CHECK(restored.settingValue("s", "chan") == "value");
}

STIPPLE_TEST(ScriptSettings, EditingTheSourceKeepsValuesWhoseFieldSurvived) {
    // Saving an edit must not silently wipe somebody's channel ID. The value
    // lives in the store, which a replacement carries over; only fields that
    // stopped being declared go quiet.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("s", "S", R"BE(
# @config chan text "Channel"
class App
  def draw()
  end
end
return App()
)BE") == ScriptPutResult::Added);
    STIPPLE_REQUIRE(store.setSetting("s", "chan", "UCkept"));

    const std::string blob = store.serialize();
    ScriptStore reloaded;
    STIPPLE_REQUIRE(reloaded.deserialize(blob));
    STIPPLE_CHECK(reloaded.settingValue("s", "chan") == "UCkept");
}
