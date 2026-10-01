// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "stipple/script/ScriptConfig.h"

namespace stipple {

class Canvas;

namespace platform {
class IAudioOutput;
class IMicrophone;
}

namespace script {

class IScriptMqtt;
class IScriptHttp;

/// One script, as the rest of the firmware sees it.
///
/// No Berry types here on purpose. This struct crosses into the core, which
/// cannot link an interpreter, so it carries text and numbers and nothing
/// else.
struct Script {
    std::string id;
    std::string name;
    std::string source;

    /// Whether it compiled and has not since failed.
    bool ok = false;

    /// Why it is not ok. Empty when it is.
    ///
    /// Kept rather than only logged, because this is the one piece of
    /// information the author is actually waiting for. The editor shows it
    /// next to the code that caused it.
    std::string problem;

    /// Instructions the last frame spent. A script close to the budget is one
    /// about to break on a busier frame, and that is worth seeing before it
    /// does.
    std::uint32_t lastInstructions = 0;

    /// Bytes its interpreter is holding.
    std::size_t memoryBytes = 0;
};

/// What a script can see of the device around it.
///
/// Pushed in by the host before each frame rather than read out by the script,
/// because the core owns the clock and the power source and a script must not
/// reach past the interface to them.
///
/// Every value that can be absent says so. A device with no battery reporting
/// 0% and a device with a flat battery look identical to a script, and one of
/// those is a lie - so `batteryKnown` is false rather than `battery` being a
/// plausible zero, and a script that cares can say "no battery" instead of
/// drawing an empty gauge. Same for the wall clock before the first time
/// sync. ADR 0013.
struct ScriptEnvironment {
    /// Local time. Only meaningful when timeKnown.
    int hour = 0;
    int minute = 0;
    int second = 0;

    int day = 1;
    int month = 1;
    int year = 1970;

    /// 0 is Sunday, matching every other weekday convention on the device.
    int weekday = 0;

    /// False until the device has a wall clock it believes in. A script that
    /// draws 00:00 on a device that does not know the time has invented it.
    bool timeKnown = false;

    int batteryPercent = 0;
    bool batteryKnown = false;
    bool charging = false;

    /// Milliseconds since the device started. Never goes backwards.
    ///
    /// This is what `now_ms()` returns, and the distinction matters more than
    /// it looks. Scripts throttle with `if now_ms() - self.last >= 250`, and
    /// that pattern needs a clock that keeps counting while the app is off
    /// screen. A clock that restarted whenever the app came round would leave
    /// `self.last` holding a number from the future, and the comparison would
    /// stay false for as long as the previous showing lasted - the script
    /// would simply stop moving.
    ///
    /// Time since the app appeared is `elapsed_ms()`, which is the right
    /// clock for an animation that should start from the beginning each time.
    std::uint64_t monotonicMillis = 0;
};

enum class ScriptPutResult : std::uint8_t {
    Added,
    Replaced,
    InvalidId,
    SourceTooLarge,
    TooManyScripts,
    /// Stored, but it does not run. Deliberately not an error: the source is
    /// saved and `problem` says what is wrong, so the author can fix it in
    /// place rather than losing it to a missing `end`.
    DidNotCompile,
};

const char* describeScriptPut(ScriptPutResult result) noexcept;

/// What the rest of the firmware needs from scripting, and nothing more.
///
/// The core cannot link Berry. ADR 0012 keeps `stipple_core` dependency-free
/// so it compiles unchanged for host, WASM and ARM, and a language runtime is
/// exactly the kind of thing that rule exists to keep out. So scripting sits
/// behind an interface for the same reason hardware does (blueprint §53): the
/// core knows that an app may be a script, that something can draw it and that
/// the API can edit it, and knows nothing about what runs it.
///
/// A build with no scripting passes nothing. That is a supported
/// configuration, not a broken one - and per ADR 0013 the absence has to be
/// visible on the panel and honest over the API, rather than showing as an app
/// that silently draws nothing.
class IScriptRunner {
public:
    virtual ~IScriptRunner() = default;

    // --- rendering -----------------------------------------------------------

    /// Whether a script with this id exists at all.
    virtual bool has(std::string_view id) const noexcept = 0;

    /// Draw one frame. False when it is missing or has failed.
    virtual bool draw(std::string_view id, Canvas& canvas, std::uint64_t elapsedMillis) = 0;

    /// Offer a button press to a script.
    ///
    /// True when the script has an `on_button(name)` and it ran. False when it
    /// has none, so the press falls through to whatever it would normally have
    /// done - a script that does not want the button must not swallow it.
    ///
    /// By default only the action button is offered, under the name "select".
    /// A script that declares `# @input exclusive` also receives "minus",
    /// "plus", "left" and "right" - see inputMode() and ADR 0024. The middle
    /// button is never offered to any script in any mode: it is how somebody
    /// leaves, and a script able to take it would be a script you could not
    /// leave. The stopwatch made the same call for the same reason.
    virtual bool button(std::string_view id, std::string_view name) = 0;

    /// How much of the panel's input this script has asked for.
    ///
    /// ActionOnly for anything missing or that did not ask, which is what
    /// makes this safe to consult unconditionally: every script written
    /// before ADR 0024 keeps the behaviour it was written against.
    virtual InputMode inputMode(std::string_view id) const = 0;

    /// How long a script would like on screen, in milliseconds.
    ///
    /// Zero means it did not say, and the carousel's own setting applies. A
    /// script that cycles through three readouts needs longer than the
    /// default and nothing else can know that.
    virtual std::uint32_t durationMillis(std::string_view id) = 0;

    /// Give scripts the speaker, or take it away.
    ///
    /// Null is a device with no audio, and the builtins then do nothing and
    /// say so - `tone()` returns false rather than pretending. A script can
    /// check once and draw a mute symbol instead of bleeping at a panel that
    /// cannot bleep.
    ///
    /// Passed in rather than reached for, like everything else here: the
    /// script layer does not own the speaker and must not decide when the
    /// device has one.
    virtual void setAudio(platform::IAudioOutput* audio) noexcept = 0;

    /// Give scripts the broker, or take it away.
    ///
    /// Null is a device with MQTT switched off or unconfigured, and then
    /// `mqtt_known()` is false and nothing publishes. Same contract as the
    /// speaker: the absence is visible to the script rather than showing up
    /// as messages that go nowhere.
    virtual void setMqtt(IScriptMqtt* mqtt) noexcept = 0;

    /// Give scripts the network, or take it away.
    ///
    /// Null is a build or a platform that cannot fetch, and then
    /// `http_known()` is false and nothing is requested. Third capability,
    /// same contract: the absence is visible to the script.
    virtual void setHttp(IScriptHttp* http) noexcept = 0;

    /// Give scripts the microphone, or take it away.
    ///
    /// Null is a device that cannot hear. Not the same as a quiet room, and
    /// the builtins keep them apart - a visualiser on a deaf device should
    /// say so rather than drawing a flatline that looks like a bug.
    virtual void setMicrophone(platform::IMicrophone* microphone) noexcept = 0;

    /// Tell every script what the device currently knows.
    ///
    /// Set once per frame by the host, before anything draws. Cheap enough to
    /// do unconditionally - it is a struct copy - and doing it unconditionally
    /// means there is no path where a script reads a stale clock because
    /// somebody forgot a call.
    virtual void setEnvironment(const ScriptEnvironment& environment) noexcept = 0;

    /// What this script has asked the device to collect for it.
    ///
    /// Declared in `# @config` comments at the top of the source and parsed
    /// from it, so there is nothing separate to keep in step: editing the
    /// declaration is editing the field.
    virtual std::vector<Setting> settings(std::string_view id) const = 0;

    /// The value a setting currently holds, rendered as text, or empty when
    /// nothing has been set and the script's own fallback applies.
    virtual std::string settingValue(std::string_view id,
                                     std::string_view key) const = 0;

    /// Set one, converting to whatever the declaration said it was.
    ///
    /// False when the script or key is unknown, or the value does not fit
    /// what was declared. A number field handed "banana" is a caller error
    /// worth reporting, not a zero worth storing.
    virtual bool setSetting(std::string_view id, std::string_view key,
                            std::string_view value) = 0;

    /// Why a script is not running, or an empty view when it is fine.
    ///
    /// The panel shows that this is non-empty; the API and the web UI show
    /// what it says. A script app that has stopped working should say so where
    /// its author can see it, rather than going black and leaving them to
    /// guess between a crash, an empty draw() and a dead device.
    virtual std::string_view problem(std::string_view id) const noexcept = 0;

    // --- editing -------------------------------------------------------------

    virtual ScriptPutResult put(std::string id, std::string name, std::string source) = 0;
    virtual bool remove(std::string_view id) = 0;
    virtual void clear() = 0;

    virtual int count() const noexcept = 0;
    virtual int capacity() const noexcept = 0;
    virtual const Script* at(int index) const noexcept = 0;
    virtual const Script* find(std::string_view id) const noexcept = 0;

    /// Total bytes every interpreter is holding, for diagnostics.
    virtual std::size_t memoryBytes() const noexcept = 0;

    /// Longest source the runner will accept, so the editor can say so before
    /// somebody pastes something too big and loses it.
    virtual std::size_t maxSourceBytes() const noexcept = 0;

    // --- persistence ---------------------------------------------------------

    /// Bumped by anything that changes the library.
    ///
    /// The host writes to storage only when this moves. Scripts live on flash
    /// and flash wears out; rewriting the whole library on every tick because
    /// nothing has changed would be a slow way to destroy the device.
    virtual std::uint32_t revision() const noexcept = 0;

    /// The whole library as one blob, and back again.
    ///
    /// deserialize() returns false on anything it does not recognise. The
    /// caller discards the blob when it does - re-reading the same broken
    /// bytes every boot turns one bad write into a permanent fault that looks
    /// intermittent.
    virtual std::string serialize() const = 0;
    virtual bool deserialize(std::string_view blob) = 0;
};

}  // namespace script
}  // namespace stipple
