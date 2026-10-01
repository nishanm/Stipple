// SPDX-License-Identifier: GPL-3.0-or-later
//
// `# @input exclusive` - a script that has asked for the controls (ADR 0024).
//
// The feature exists because one button is not a game. The *constraint* that
// makes it safe is that one control is never handed over, and that is what
// most of this file is about: a script may take the knob, both detents and
// both labelled buttons, and it may never take the middle button or a held
// knob. Those are how somebody leaves.
//
// If AMiddlePressIsNeverDeliveredToAScript is ever relaxed, the guarantee
// ADR 0024 rests on is gone and the old rule - never offer the knob at all -
// should come back instead.
#include "stipple/script/ScriptConfig.h"

#include <string>

#include "stipple/app/AppRegistry.h"
#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/host/ApplicationHost.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "stipple/script/ScriptStore.h"
#include "support/TestFramework.h"

using stipple::Canvas;
using stipple::Framebuffer;
using stipple::host::ApplicationHost;
using stipple::host::HostConfig;
using stipple::platform::RawInput;
using stipple::platform::simulator::SimulatorPlatform;
using stipple::script::InputMode;
using stipple::script::parseInputMode;
using stipple::script::ScriptPutResult;
using stipple::script::ScriptStore;

namespace {

HostConfig quietConfig() {
    HostConfig config;
    config.splashMillis = 0;
    return config;
}

/// A script that records the name of every press it is given, one pixel per
/// control, so the panel itself says which names arrived.
///
/// Reading it off the framebuffer rather than out of the store is deliberate:
/// it proves the press travelled the whole path - simulated hardware, mapper,
/// host, store, interpreter - rather than that a method returned true.
const char* kRecorder =
    "class App\n"
    "  var seen\n"
    "  def init()\n"
    "    self.seen = {}\n"
    "  end\n"
    "  def on_button(name)\n"
    "    self.seen[name] = true\n"
    "  end\n"
    "  def draw()\n"
    "    if self.seen.contains('left')   pixel(0, 0, rgb(255, 0, 0)) end\n"
    "    if self.seen.contains('right')  pixel(1, 0, rgb(255, 0, 0)) end\n"
    "    if self.seen.contains('select') pixel(2, 0, rgb(255, 0, 0)) end\n"
    "    if self.seen.contains('minus')  pixel(3, 0, rgb(255, 0, 0)) end\n"
    "    if self.seen.contains('plus')   pixel(4, 0, rgb(255, 0, 0)) end\n"
    "    if self.seen.contains('middle')  pixel(5, 0, rgb(255, 0, 0)) end\n"
    "  end\n"
    "end\n"
    "return App()\n";

std::string exclusive(const char* body) {
    return std::string("# @input exclusive\n") + body;
}

/// Host with one script pinned on screen, ready to be pressed at.
struct Rig {
    SimulatorPlatform platform;
    ApplicationHost host{platform, quietConfig()};
    ScriptStore store;

    bool start(const std::string& source) {
        if (!host.initialize()) {
            return false;
        }
        if (store.put("game", "Game", source) != ScriptPutResult::Added) {
            return false;
        }
        host.setScriptRunner(&store);

        stipple::app::App entry;
        entry.id = "game";
        entry.name = "Game";
        entry.builtin = stipple::app::Builtin::Script;
        if (host.apps().put(entry) != stipple::app::AppRegistry::PutResult::Added) {
            return false;
        }
        if (!host.carousel().pin("game", 1000)) {
            return false;
        }
        host.tick(1000);
        return true;
    }

    /// A short press, well under the 500 ms that makes it a long one.
    void tap(RawInput control, std::uint64_t at) {
        platform.simulatedInput().pressAndRelease(control, at, 80);
        host.tick(at + 150);
    }

    void turn(bool clockwise, std::uint64_t at) {
        platform.simulatedInput().rotate(clockwise, at);
        host.tick(at + 150);
    }

    /// Ask the script what it has seen, by drawing it into a canvas of our
    /// own rather than reading the panel.
    ///
    /// The panel is the wrong witness here and it took three failing tests to
    /// notice: a press the script did *not* get is one the carousel did, so
    /// the next frame shows the clock, or the settings menu, and those light
    /// pixels of their own. Reading (1, 0) off the panel then answers "is
    /// something on screen here", which is a different question and happens
    /// to be true. Drawing the script directly asks only the script.
    bool saw(int x) {
        Framebuffer framebuffer;
        Canvas canvas(framebuffer);
        if (!store.draw("game", canvas, 0)) {
            return false;
        }
        return framebuffer.at(x, 0) != stipple::colors::kBlack;
    }
};

}  // namespace

// --- the directive -----------------------------------------------------------

STIPPLE_TEST(ScriptInput, TheDirectiveParses) {
    STIPPLE_CHECK(parseInputMode("# @input exclusive\nclass App\n") == InputMode::Exclusive);
    STIPPLE_CHECK(parseInputMode("#   @input   exclusive  \n") == InputMode::Exclusive);
    STIPPLE_CHECK(parseInputMode("# name: Game\n"
                                 "# @input exclusive\n"
                                 "\n"
                                 "class App\n") == InputMode::Exclusive);
}

STIPPLE_TEST(ScriptInput, AScriptThatDoesNotAskGetsTheOldBehaviour) {
    STIPPLE_CHECK(parseInputMode("class App\n") == InputMode::ActionOnly);
    STIPPLE_CHECK(parseInputMode("") == InputMode::ActionOnly);
    STIPPLE_CHECK(parseInputMode("# name: Plain\n# summary: nothing\n") == InputMode::ActionOnly);
}

STIPPLE_TEST(ScriptInput, ADirectiveBelowTheHeaderIsAComment) {
    // Same rule as @config: a directive written halfway down the file is a
    // comment about the code beside it, and honouring it would be a guess.
    STIPPLE_CHECK(parseInputMode("class App\n"
                                 "  # @input exclusive\n"
                                 "end\n") == InputMode::ActionOnly);
}

STIPPLE_TEST(ScriptInput, AnUnknownModeLeavesTheScriptRunning) {
    // A script asking for a mode this firmware has never heard of must still
    // work. The alternative strands a newer script on an older device for the
    // sake of a word in a comment.
    STIPPLE_CHECK(parseInputMode("# @input telepathy\nclass App\n") == InputMode::ActionOnly);
    STIPPLE_CHECK(parseInputMode("# @input\nclass App\n") == InputMode::ActionOnly);
    STIPPLE_CHECK(parseInputMode("# @inputexclusive\n") == InputMode::ActionOnly);
}

STIPPLE_TEST(ScriptInput, TheStoreRemembersWhatTheHeaderSaid) {
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("a", "A", exclusive(kRecorder)) == ScriptPutResult::Added);
    STIPPLE_REQUIRE(store.put("b", "B", kRecorder) == ScriptPutResult::Added);

    STIPPLE_CHECK(store.inputMode("a") == InputMode::Exclusive);
    STIPPLE_CHECK(store.inputMode("b") == InputMode::ActionOnly);
    // A script that is not there cannot have asked for anything.
    STIPPLE_CHECK(store.inputMode("nobody") == InputMode::ActionOnly);
}

STIPPLE_TEST(ScriptInput, SavingAnEditReparsesTheDirective) {
    // The mode is cached on the entry, so the cache has to be rebuilt when the
    // source changes - otherwise removing the directive leaves the controls
    // taken by a script that no longer asks for them.
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("a", "A", exclusive(kRecorder)) == ScriptPutResult::Added);
    STIPPLE_CHECK(store.inputMode("a") == InputMode::Exclusive);

    STIPPLE_REQUIRE(store.put("a", "A", kRecorder) == ScriptPutResult::Replaced);
    STIPPLE_CHECK(store.inputMode("a") == InputMode::ActionOnly);
}

STIPPLE_TEST(ScriptInput, TheModeSurvivesBeingSavedAndLoaded) {
    ScriptStore store;
    STIPPLE_REQUIRE(store.put("a", "A", exclusive(kRecorder)) == ScriptPutResult::Added);

    ScriptStore restored;
    STIPPLE_REQUIRE(restored.deserialize(store.serialize()));
    STIPPLE_CHECK(restored.inputMode("a") == InputMode::Exclusive);
}

// --- through the whole device ------------------------------------------------

STIPPLE_TEST(ScriptInput, AnExclusiveScriptReceivesEveryControlItIsOwed) {
    Rig rig;
    STIPPLE_REQUIRE(rig.start(exclusive(kRecorder)));

    rig.turn(false, 1100);
    rig.turn(true, 1400);
    rig.tap(RawInput::RotaryPress, 1700);
    rig.tap(RawInput::KeyMinus, 2000);
    rig.tap(RawInput::KeyPlus, 2300);

    STIPPLE_CHECK(rig.saw(0));  // left
    STIPPLE_CHECK(rig.saw(1));  // right
    STIPPLE_CHECK(rig.saw(2));  // select
    STIPPLE_CHECK(rig.saw(3));  // minus
    STIPPLE_CHECK(rig.saw(4));  // plus
}

STIPPLE_TEST(ScriptInput, AMiddlePressIsNeverDeliveredToAScript) {
    // **This is the test the whole design rests on.** The middle button is
    // Action::Back in every mode (ADR 0017), which is what makes it safe to
    // give a script everything else. A script able to take it would be a
    // script you could not leave, on a device with no other way out.
    Rig rig;
    STIPPLE_REQUIRE(rig.start(exclusive(kRecorder)));

    // First, prove the handler is listening at all, so a later silence means
    // something. A press the script is allowed to have arrives.
    rig.tap(RawInput::RotaryPress, 1100);
    STIPPLE_REQUIRE(rig.saw(2));

    rig.tap(RawInput::KeyMiddle, 1500);

    // It never reached on_button under any name.
    STIPPLE_CHECK_FALSE(rig.saw(5));

    // And it did the thing it is reserved for. Back returns to the clock, so
    // a script that had taken every other control is no longer the app on
    // screen - which is the entire escape hatch, demonstrated rather than
    // asserted about the binding table.
    const stipple::app::App* active = rig.host.carousel().active();
    STIPPLE_REQUIRE(active != nullptr);
    STIPPLE_CHECK(active->id != "game");
}

STIPPLE_TEST(ScriptInput, TheKnobStillMovesBetweenAppsForAnOrdinaryScript) {
    // Every script written before ADR 0024 keeps exactly the behaviour it was
    // written against. A dozen shipped scripts have an on_button that toggles
    // on any press, and routing detents into those would break them in a way
    // that reads as a firmware bug.
    Rig rig;
    STIPPLE_REQUIRE(rig.start(kRecorder));  // no directive

    rig.turn(false, 1100);
    rig.turn(true, 1400);
    rig.tap(RawInput::KeyMinus, 1700);
    rig.tap(RawInput::KeyPlus, 2000);

    STIPPLE_CHECK_FALSE(rig.saw(0));
    STIPPLE_CHECK_FALSE(rig.saw(1));
    STIPPLE_CHECK_FALSE(rig.saw(3));
    STIPPLE_CHECK_FALSE(rig.saw(4));
}

STIPPLE_TEST(ScriptInput, AnExclusiveScriptWithNoHandlerDoesNotEatTheControls) {
    // Declaring @input and forgetting on_button would otherwise produce an app
    // with five dead controls and nothing on screen to explain it. The press
    // falls through to its ordinary job instead.
    Rig rig;
    STIPPLE_REQUIRE(rig.start(exclusive(
        "class App\n"
        "  def draw()\n"
        "    pixel(0, 0, rgb(0, 255, 0))\n"
        "  end\n"
        "end\n"
        "return App()\n")));

    const bool pausedBefore = rig.host.carousel().paused();
    rig.tap(RawInput::RotaryPress, 1100);
    STIPPLE_CHECK(rig.host.carousel().paused() != pausedBefore);
}

STIPPLE_TEST(ScriptInput, HoldingTheKnobStillReachesSettings) {
    // The other reserved gesture. A long press is SettingsToggle, and a script
    // cannot have it for the same reason it cannot have the middle button.
    Rig rig;
    STIPPLE_REQUIRE(rig.start(exclusive(kRecorder)));

    rig.platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 1100, 900);
    rig.host.tick(2200);

    STIPPLE_CHECK(rig.host.navigator().inSettings());
    // And it was not also delivered as a press.
    STIPPLE_CHECK_FALSE(rig.saw(2));
}

STIPPLE_TEST(ScriptInput, TheControlsGoBackToTheCarouselInsideSettings) {
    // A script does not get to fight the settings menu for the knob while
    // somebody is trying to change the brightness.
    Rig rig;
    STIPPLE_REQUIRE(rig.start(exclusive(kRecorder)));

    rig.platform.simulatedInput().pressAndRelease(RawInput::RotaryPress, 1100, 900);
    rig.host.tick(2200);
    STIPPLE_REQUIRE(rig.host.navigator().inSettings());

    rig.turn(true, 2400);
    STIPPLE_CHECK_FALSE(rig.saw(1));
}
