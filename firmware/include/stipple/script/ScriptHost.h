// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "stipple/script/IScriptRunner.h"

namespace stipple {

class Canvas;

namespace script {

/// Runs one Berry script against the panel.
///
/// A script is a Berry class with a `draw()` method, the shape AWTRIX NG
/// documents and the shape existing scripts are written in. The host compiles
/// the source, constructs the class once, and calls `draw()` on the frames it
/// is on screen for.
///
/// **The script is untrusted.** It arrives over the network from whoever can
/// reach the device, and it runs on the thread that draws the panel. Three
/// things follow, and none of them are optional:
///
///   - The interpreter is built without a filesystem, a dynamic loader or a
///     bytecode reader (firmware/script/berry_conf.h), so the only things a
///     script can reach are the builtins registered here.
///   - Every call is bounded by an instruction budget. A script that loops
///     for ever loses its frame; the device keeps rendering.
///   - A script that fails is disabled rather than retried every frame. A
///     broken script that throws thirty times a second would fill the log and
///     starve everything else of time.
class ScriptHost {
public:
    /// Instructions a single call may spend.
    ///
    /// Generous for anything drawing a 52x16 panel and far short of a frame's
    /// worth of time on this hardware, which is the balance wanted: an honest
    /// script should never meet it, and a runaway one should meet it quickly.
    static constexpr std::uint32_t kInstructionBudget = 200000;

    /// Longest source a script may have. Bounded like everything else here -
    /// this arrives over the network and is held in RAM.
    static constexpr std::size_t kMaxSourceBytes = 16u * 1024u;

    ScriptHost();
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Compile and instantiate. False leaves `problem` describing why, with a
    /// line number where Berry gave one - a compile error without one is
    /// useless in the editor that just showed the author their mistake.
    bool load(std::string_view source, std::string& problem);

    /// Run the script's `draw()` for one frame.
    ///
    /// False means the script failed and has been disabled; `problem` says
    /// what happened. The canvas may have been partly drawn to, which is
    /// deliberate - a script that dies half way through has still told you
    /// something, and blanking it would hide the evidence.
    bool draw(Canvas& canvas, std::uint64_t elapsedMillis, std::string& problem);

    /// What happened when an optional callback was offered to a script.
    ///
    /// Three outcomes rather than a bool, because "the script has no
    /// on_button" and "the script's on_button threw" need different
    /// responses: the first should let the press fall through to the
    /// carousel, and the second must not.
    enum class EventResult {
        NotDefined,
        Handled,
        Failed,
    };

    /// Offer a button press to the script's `on_button(name)`.
    ///
    /// Bounded by the same instruction budget as draw(), and a failure
    /// disables the script the same way - a handler that loops for ever is
    /// exactly as bad as a draw() that does, and arrives by the same route.
    EventResult button(std::string_view name, std::string& problem);

    /// One value a script has asked the device to remember.
    ///
    /// Tagged rather than everything-is-a-string, because `store.get("sound",
    /// false)` has to give back a boolean and a script comparing it to `false`
    /// would otherwise get the string "false", which Berry considers true.
    struct Stored {
        enum class Kind : std::uint8_t { Integer, Real, Boolean, Text };
        Kind kind = Kind::Integer;
        std::int32_t integer = 0;
        float real = 0.0f;
        bool boolean = false;
        std::string text;
    };

    /// Bounded like everything else. A script that writes a new key every
    /// frame would otherwise fill the device with its own rubbish, and it
    /// would do it slowly enough that nobody would connect the two.
    static constexpr std::size_t kMaxStoreKeys = 16;
    static constexpr std::size_t kMaxStoreKeyBytes = 32;
    static constexpr std::size_t kMaxStoreTextBytes = 128;

    /// The script's remembered values, in insertion order, for persistence.
    const std::vector<std::pair<std::string, Stored>>& stored() const noexcept;
    void restoreStored(std::vector<std::pair<std::string, Stored>> values);

    /// Set one remembered value from outside the script.
    ///
    /// How a `@config` setting reaches the code that reads it: the web UI
    /// writes here, and the script picks it up with the `store.get(key,
    /// fallback)` it was already using. One namespace, so a script running on
    /// a firmware too old to know about `@config` still works - it just falls
    /// back on its own.
    ///
    /// False when the key is unusable or the store is full.
    bool setStored(std::string_view key, Stored value);

    /// Ask the script how long it would like on screen, in milliseconds.
    ///
    /// Zero when it has no `duration()`, which means the carousel's own
    /// setting applies. A script that wants longer has to say so - a moon
    /// phase cycling through three readouts needs more than the default five
    /// seconds, and nothing else can know that.
    std::uint32_t durationMillis();

    /// What this script sees of the device. Read by the time and battery
    /// builtins; set by the host before each frame.
    void setEnvironment(const ScriptEnvironment& environment) noexcept;

    /// The speaker this script may use, or null on a device without one.
    void setAudio(platform::IAudioOutput* audio) noexcept;

    /// The broker this script may use, or null when there is none.
    ///
    /// The id comes with it, because every call through `IScriptMqtt` is
    /// scoped by script: a script publishes under its own subtree and reads
    /// its own watches, and the host is the only thing that knows which
    /// script this is.
    void setMqtt(IScriptMqtt* mqtt, std::string_view scriptId);

    /// The network this script may use, or null when there is none. Scoped by
    /// id for the same reason the broker is.
    void setHttp(IScriptHttp* http, std::string_view scriptId);

    /// The microphone this script may read, or null on a device that cannot
    /// hear. One amplitude, not a spectrum - see IMicrophone.
    void setMicrophone(platform::IMicrophone* microphone) noexcept;

    /// Whether a script is loaded and has not failed.
    bool ready() const noexcept { return ready_; }

    /// Instructions the last call spent, for the diagnostics the web UI
    /// shows. A script that is close to the budget is one about to break on a
    /// slower frame.
    std::uint32_t lastInstructions() const noexcept { return lastInstructions_; }

    /// Bytes this script's interpreter is holding.
    ///
    /// Berry's own accounting, not an estimate. It exists because "how much
    /// RAM does a script cost" is the question that decides how many scripts
    /// a 36 MB device can run, and answering it by guessing is how you find
    /// out on the device rather than in a test.
    std::size_t memoryBytes() const noexcept;

    /// Force a full collection and report what is actually still live.
    ///
    /// memoryBytes() alone sawtooths: Berry is garbage collected, so between
    /// collections the number climbs with every temporary a frame makes. The
    /// difference matters - a rising sawtooth is normal and a rising floor is
    /// a leak, and only this call can tell them apart.
    ///
    /// Explicit rather than folded into memoryBytes() because collecting is
    /// work, and the host wants to choose when to do it: when a script leaves
    /// the screen, not in the middle of the frame it is drawing.
    std::size_t collectGarbage() noexcept;

private:
    /// Call an optional method on the script's instance.
    ///
    /// One place, because this is where the stack accounting lives and a
    /// second copy of it is a second chance to get it wrong.
    EventResult invoke(const char* method, const char* argument, std::string& problem);

    struct State;
    State* state_;
    bool ready_ = false;
    std::uint32_t lastInstructions_ = 0;
    ScriptEnvironment environment_;
    platform::IAudioOutput* audio_ = nullptr;
    IScriptMqtt* mqtt_ = nullptr;
    IScriptHttp* http_ = nullptr;
    platform::IMicrophone* microphone_ = nullptr;
    std::string scriptId_;
    std::vector<std::pair<std::string, Stored>> store_;
};

}  // namespace script
}  // namespace stipple
