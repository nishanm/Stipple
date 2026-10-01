// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/script/ScriptHost.h"

#include "stipple/script/IScriptHttp.h"
#include "stipple/script/IScriptMqtt.h"

extern "C" {
#include "berry.h"
#include "be_gc.h"
}

#include <cstring>

#include "stipple/core/Rgb.h"
#include "stipple/platform/PlatformServices.h"
#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/text/Text.h"

namespace stipple {
namespace script {
namespace {

/// The script currently drawing.
///
/// A file-static rather than something carried through Berry, because Berry's
/// native functions receive only a `bvm*` and the render loop is single
/// threaded - there is exactly one script drawing at any moment, and it is
/// this one. Set for the duration of a call and cleared after, so a builtin
/// reached any other way finds nothing and does nothing.
struct Active {
    Canvas* canvas = nullptr;
    std::uint64_t elapsedMillis = 0;
    std::uint32_t heartbeats = 0;
    bool overBudget = false;
    ScriptEnvironment environment;

    /// The drawing script's own remembered values. Null outside a call, so a
    /// builtin reached any other way writes nothing.
    std::vector<std::pair<std::string, ScriptHost::Stored>>* store = nullptr;

    /// The speaker, for the duration of a call. Null on a device without one,
    /// and null outside a call.
    platform::IAudioOutput* audio = nullptr;

    /// Tones asked for during this call. Bounded - see b_tone.
    int tones = 0;

    /// The broker, for the duration of a call, and the id this script is
    /// known by. Every call through IScriptMqtt is scoped by that id, so a
    /// script cannot publish as another one or read another's watches.
    IScriptMqtt* mqtt = nullptr;
    const std::string* scriptId = nullptr;

    /// Messages published during this call. Bounded - see b_mqtt_publish.
    int publishes = 0;

    /// The network, for the duration of a call. Scoped by the same script id
    /// as the broker.
    IScriptHttp* http = nullptr;

    /// The microphone, for the duration of a call. Null on a device that
    /// cannot hear, which is not the same as a quiet room.
    platform::IMicrophone* microphone = nullptr;
};

Active g_active;

/// The instruction budget.
///
/// Berry calls this every 2^BE_VM_OBSERVABILITY_SAMPLING instructions. Raising
/// from here unwinds the script the same way any other error does, which is
/// what the hook is for - a runaway loop loses its frame and the panel keeps
/// rendering.
void observe(bvm* vm, int event, ...) {
    if (event != BE_OBS_VM_HEARTBEAT) {
        return;
    }
    // Sampling is 2^16, so each heartbeat is 65536 instructions.
    g_active.heartbeats += 1;
    const std::uint32_t spent = g_active.heartbeats * 65536u;
    if (spent >= ScriptHost::kInstructionBudget) {
        g_active.overBudget = true;
        be_raise(vm, "stipple_budget", "the script ran too long for one frame");
    }
}

Rgb fromScriptColor(bint packed) {
    const std::uint32_t value = static_cast<std::uint32_t>(packed);
    return rgb(static_cast<std::uint8_t>((value >> 16) & 0xFFu),
               static_cast<std::uint8_t>((value >> 8) & 0xFFu),
               static_cast<std::uint8_t>(value & 0xFFu));
}

int argInt(bvm* vm, int index, int fallback = 0) {
    if (be_isint(vm, index)) {
        return static_cast<int>(be_toint(vm, index));
    }
    if (be_isreal(vm, index)) {
        return static_cast<int>(be_toreal(vm, index));
    }
    return fallback;
}

/* --- the builtins --------------------------------------------------------
 *
 * Named to match AWTRIX NG's documented scripting API, so a script written
 * against that runs here unchanged. The names are an interface reimplemented
 * from its specification; no AWTRIX source was read.
 */

int b_width(bvm* vm) {
    be_pushint(vm, Framebuffer::kWidth);
    be_return(vm);
}

int b_height(bvm* vm) {
    be_pushint(vm, Framebuffer::kHeight);
    be_return(vm);
}

int b_rgb(bvm* vm) {
    const int top = be_top(vm);
    const int r = top >= 1 ? argInt(vm, 1) : 0;
    const int g = top >= 2 ? argInt(vm, 2) : 0;
    const int b = top >= 3 ? argInt(vm, 3) : 0;
    const auto clamp8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    be_pushint(vm, (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b));
    be_return(vm);
}

int b_clear(bvm* vm) {
    if (g_active.canvas != nullptr) {
        const Rgb colour = be_top(vm) >= 1 ? fromScriptColor(be_toint(vm, 1)) : colors::kBlack;
        g_active.canvas->clear(colour);
    }
    be_return_nil(vm);
}

int b_pixel(bvm* vm) {
    if (g_active.canvas != nullptr && be_top(vm) >= 3) {
        // The canvas clips, so coordinates from a script are bounded by the
        // panel rather than trusted.
        g_active.canvas->pixel(argInt(vm, 1), argInt(vm, 2), fromScriptColor(be_toint(vm, 3)));
    }
    be_return_nil(vm);
}

int b_line(bvm* vm) {
    if (g_active.canvas != nullptr && be_top(vm) >= 5) {
        g_active.canvas->line(argInt(vm, 1), argInt(vm, 2), argInt(vm, 3), argInt(vm, 4),
                              fromScriptColor(be_toint(vm, 5)));
    }
    be_return_nil(vm);
}

int b_rect(bvm* vm) {
    if (g_active.canvas != nullptr && be_top(vm) >= 5) {
        g_active.canvas->rect(Rect{argInt(vm, 1), argInt(vm, 2), argInt(vm, 3), argInt(vm, 4)},
                              fromScriptColor(be_toint(vm, 5)));
    }
    be_return_nil(vm);
}

int b_rect_fill(bvm* vm) {
    if (g_active.canvas != nullptr && be_top(vm) >= 5) {
        g_active.canvas->fillRect(
            Rect{argInt(vm, 1), argInt(vm, 2), argInt(vm, 3), argInt(vm, 4)},
            fromScriptColor(be_toint(vm, 5)));
    }
    be_return_nil(vm);
}

int b_text(bvm* vm) {
    int advance = 0;
    if (g_active.canvas != nullptr && be_top(vm) >= 3 && be_isstring(vm, 3)) {
        const char* body = be_tostring(vm, 3);
        const Rgb colour =
            be_top(vm) >= 4 ? fromScriptColor(be_toint(vm, 4)) : colors::kWhite;
        const int x = argInt(vm, 1);
        const int y = argInt(vm, 2);
        text::drawLine(*g_active.canvas, body, x, y, text::font5x7(), colour);
        advance = text::measureLine(body, text::font5x7());
    }
    // AWTRIX NG's text() returns the pixel advance, which scripts use to lay
    // the next thing out.
    be_pushint(vm, advance);
    be_return(vm);
}

int b_text_width(bvm* vm) {
    int width = 0;
    if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
        width = text::measureLine(be_tostring(vm, 1), text::font5x7());
    }
    be_pushint(vm, width);
    be_return(vm);
}

/* --- what the device knows --------------------------------------------------
 *
 * Each of these has a companion that says whether the value means anything.
 * A script that draws 00:00 on a device which has never synchronised its clock
 * has invented the time, and one that draws an empty gauge on a device with no
 * battery has invented the battery. ADR 0013: absence has to be visible, not
 * dressed up as a plausible zero.
 */

int b_hour(bvm* vm) {
    be_pushint(vm, g_active.environment.hour);
    be_return(vm);
}

int b_minute(bvm* vm) {
    be_pushint(vm, g_active.environment.minute);
    be_return(vm);
}

int b_second(bvm* vm) {
    be_pushint(vm, g_active.environment.second);
    be_return(vm);
}

int b_weekday(bvm* vm) {
    be_pushint(vm, g_active.environment.weekday);
    be_return(vm);
}

int b_day(bvm* vm) {
    be_pushint(vm, g_active.environment.day);
    be_return(vm);
}

int b_month(bvm* vm) {
    be_pushint(vm, g_active.environment.month);
    be_return(vm);
}

int b_year(bvm* vm) {
    be_pushint(vm, g_active.environment.year);
    be_return(vm);
}

int b_time_known(bvm* vm) {
    be_pushbool(vm, g_active.environment.timeKnown ? 1 : 0);
    be_return(vm);
}

int b_battery(bvm* vm) {
    be_pushint(vm, g_active.environment.batteryPercent);
    be_return(vm);
}

int b_battery_known(bvm* vm) {
    be_pushbool(vm, g_active.environment.batteryKnown ? 1 : 0);
    be_return(vm);
}

int b_charging(bvm* vm) {
    be_pushbool(vm, g_active.environment.charging ? 1 : 0);
    be_return(vm);
}

/* --- remembering things -----------------------------------------------------
 *
 * `store.get(key, fallback)` and `store.set(key, value)`, which is how the
 * scripts people have already written keep a high score or a preference.
 *
 * The native half is two plain functions. The `store` object a script actually
 * calls is built by a few lines of Berry at start-up (see kPrelude), because
 * defining a class from C is a page of table-building for something the
 * language expresses in five lines - and those five lines cost about a hundred
 * bytes in a VM that costs four thousand.
 */

ScriptHost::Stored* findStored(const char* key) {
    if (g_active.store == nullptr || key == nullptr) {
        return nullptr;
    }
    for (auto& entry : *g_active.store) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

void pushStored(bvm* vm, const ScriptHost::Stored& value) {
    switch (value.kind) {
        case ScriptHost::Stored::Kind::Integer:
            be_pushint(vm, value.integer);
            return;
        case ScriptHost::Stored::Kind::Real:
            be_pushreal(vm, value.real);
            return;
        case ScriptHost::Stored::Kind::Boolean:
            be_pushbool(vm, value.boolean ? 1 : 0);
            return;
        case ScriptHost::Stored::Kind::Text:
            be_pushstring(vm, value.text.c_str());
            return;
    }
    be_pushnil(vm);
}

int b_store_get(bvm* vm) {
    const int top = be_top(vm);
    if (top >= 1 && be_isstring(vm, 1)) {
        if (const ScriptHost::Stored* found = findStored(be_tostring(vm, 1));
            found != nullptr) {
            pushStored(vm, *found);
            be_return(vm);
        }
    }

    // Nothing stored: hand back the fallback the script supplied, whatever it
    // is. Returning nil instead would make every script write the same three
    // lines of defaulting.
    if (top >= 2) {
        be_pushvalue(vm, 2);
    } else {
        be_pushnil(vm);
    }
    be_return(vm);
}

int b_store_set(bvm* vm) {
    if (g_active.store == nullptr || be_top(vm) < 2 || !be_isstring(vm, 1)) {
        be_return_nil(vm);
    }

    const char* key = be_tostring(vm, 1);
    if (key == nullptr || key[0] == 0 ||
        std::strlen(key) > ScriptHost::kMaxStoreKeyBytes) {
        be_return_nil(vm);
    }

    ScriptHost::Stored value;
    if (be_isint(vm, 2)) {
        value.kind = ScriptHost::Stored::Kind::Integer;
        value.integer = static_cast<std::int32_t>(be_toint(vm, 2));
    } else if (be_isbool(vm, 2)) {
        value.kind = ScriptHost::Stored::Kind::Boolean;
        value.boolean = be_tobool(vm, 2) != 0;
    } else if (be_isreal(vm, 2)) {
        value.kind = ScriptHost::Stored::Kind::Real;
        value.real = static_cast<float>(be_toreal(vm, 2));
    } else if (be_isstring(vm, 2)) {
        value.kind = ScriptHost::Stored::Kind::Text;
        const char* text = be_tostring(vm, 2);
        value.text = text != nullptr ? text : "";
        if (value.text.size() > ScriptHost::kMaxStoreTextBytes) {
            value.text.resize(ScriptHost::kMaxStoreTextBytes);
        }
    } else {
        // Lists, maps and instances are not stored. They have no stable shape
        // to write to flash, and quietly storing something else under the key
        // would be worse than refusing.
        be_return_nil(vm);
    }

    if (ScriptHost::Stored* existing = findStored(key); existing != nullptr) {
        *existing = std::move(value);
        be_return_nil(vm);
    }

    // Full. Refused rather than evicting something: a script cannot be
    // expected to guess which of its own keys the device threw away.
    if (g_active.store->size() >= ScriptHost::kMaxStoreKeys) {
        be_return_nil(vm);
    }

    g_active.store->emplace_back(key, std::move(value));
    be_return_nil(vm);
}

/* --- scrolling text ---------------------------------------------------------
 *
 * A panel eight characters wide needs this for almost any real message, and
 * the scripts people have written already call it.
 *
 * Returns the number of complete passes, which is the only way a script can
 * know its message has been read - Selenograph uses exactly that to decide
 * when to move on from a scrolling caption to the next thing.
 */
int b_scroll_text(bvm* vm) {
    int laps = 0;
    if (g_active.canvas != nullptr && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        const char* body = be_tostring(vm, 1);
        const Rgb colour = be_top(vm) >= 2 ? fromScriptColor(be_toint(vm, 2)) : colors::kWhite;
        const int y = be_top(vm) >= 3 ? argInt(vm, 3) : (Framebuffer::kHeight - 7) / 2;

        const int ink = text::measureLine(body, text::font5x7());

        // One pass is the text entering from the right and leaving on the
        // left, so the cycle is the panel plus the text. Measured in pixels
        // travelled rather than in frames, so the speed is the same whatever
        // the frame rate - and identical in the emulator and on the device.
        const int cycle = Framebuffer::kWidth + ink;
        if (cycle > 0) {
            constexpr int kPixelsPerSecond = 14;  // slow enough to read
            const int travelled =
                static_cast<int>((g_active.elapsedMillis * kPixelsPerSecond) / 1000u);
            laps = travelled / cycle;
            const int x = Framebuffer::kWidth - (travelled % cycle);
            text::drawLine(*g_active.canvas, body, x, y, text::font5x7(), colour);
        }
    }
    be_pushint(vm, laps);
    be_return(vm);
}

/// Milliseconds since the device started.
///
/// Monotonic, not per-showing. Scripts throttle with
/// `if now_ms() - self.last >= 250`, and that needs a clock that keeps
/// counting while the app is off screen: a clock that restarted would leave
/// `self.last` holding a number from the future and the script would stop
/// moving until the clock caught up.
///
/// This was per-showing to begin with, and the aquarium's fish froze the
/// first time the carousel came back round to them.
/* --- the speaker -------------------------------------------------------------
 *
 * playTone and playSound are non-blocking by contract (blueprint §16): they
 * queue and return, because audio must never hold up the panel. That is what
 * makes them safe to hand to a script at all.
 *
 * Every one of these reports whether it actually did anything. A device with
 * no speaker returns false rather than silently succeeding, so a script can
 * draw a mute symbol instead of bleeping at something that cannot bleep.
 */

/// Tones a script may start in one call.
///
/// A script calling tone() in a loop would otherwise queue thousands, and the
/// panel would be fine while the speaker worked through a minute of them. Four
/// is enough for a chord or a short arpeggio, which is what anything drawing a
/// 52x16 panel is plausibly doing.
constexpr int kMaxTonesPerCall = 4;

int b_tone(bvm* vm) {
    bool started = false;
    if (g_active.audio != nullptr && be_top(vm) >= 2 &&
        g_active.tones < kMaxTonesPerCall) {
        const int hz = argInt(vm, 1);
        const int ms = argInt(vm, 2);
        // Bounded on the way in. A script asking for an hour-long tone has
        // made a mistake, and the speaker should not be the thing that finds
        // out.
        if (hz > 0 && hz <= 20000 && ms > 0) {
            started = g_active.audio->playTone(hz, ms > 5000 ? 5000 : ms);
            g_active.tones += 1;
        }
    }
    be_pushbool(vm, started ? 1 : 0);
    be_return(vm);
}

int b_sound(bvm* vm) {
    bool played = false;
    if (g_active.audio != nullptr && be_top(vm) >= 1 && be_isstring(vm, 1) &&
        g_active.tones < kMaxTonesPerCall) {
        played = g_active.audio->playSound(be_tostring(vm, 1));
        g_active.tones += 1;
    }
    be_pushbool(vm, played ? 1 : 0);
    be_return(vm);
}

int b_audio_known(bvm* vm) {
    be_pushbool(vm, g_active.audio != nullptr ? 1 : 0);
    be_return(vm);
}

int b_volume(bvm* vm) {
    // Read-only. The volume is the user's setting, made on the device or in
    // its web page, and an app quietly turning it up is not a feature.
    be_pushint(vm, g_active.audio != nullptr ? g_active.audio->volume() : 0);
    be_return(vm);
}

/* --- the broker ------------------------------------------------------------
 *
 * Five builtins, and the shape of them is dictated by the shape of a script.
 * A script is awake for a few milliseconds every few seconds; it cannot be
 * holding a subscription callback, and it cannot wait for a reply. So what it
 * gets is "tell me the last thing said on this topic", answered from a cache
 * the gateway keeps, plus a publish that either goes now or says it did not.
 *
 * mqtt_get() returning nil for "nothing has arrived" rather than an empty
 * string is the detail that earns its keep: a sensor that publishes "" and a
 * sensor that has said nothing since the device booted are different states,
 * and a script that cannot tell them apart draws one as the other.
 */

/// Whether this script can reach the broker at all.
bool brokerReady() noexcept {
    return g_active.mqtt != nullptr && g_active.scriptId != nullptr &&
           !g_active.scriptId->empty();
}

/// Messages a script may publish in one call.
///
/// Two, and low on purpose. A script looping over a publish is a device
/// flooding somebody's home automation from inside their own network, and it
/// would look like the broker misbehaving rather than like a script anybody
/// would think to suspect. Anything drawing a 52x16 panel that needs to say
/// more than two things per frame is not publishing, it is shouting.
constexpr int kMaxPublishesPerCall = IScriptMqtt::kMaxPublishesPerCall;

int b_mqtt_known(bvm* vm) {
    be_pushbool(vm, (brokerReady() && g_active.mqtt->connected()) ? 1 : 0);
    be_return(vm);
}

int b_mqtt_publish(bvm* vm) {
    bool sent = false;
    if (brokerReady() && be_top(vm) >= 2 && be_isstring(vm, 1) &&
        g_active.publishes < kMaxPublishesPerCall) {
        // The payload may be a number as easily as a string - a script
        // publishing a temperature should not have to remember to convert it,
        // and be_tostring does the same thing print() would.
        const char* leaf = be_tostring(vm, 1);
        const char* payload = be_tostring(vm, 2);
        // Retained by request only. A retained message outlives the device
        // that sent it, so defaulting to it would leave a script's last
        // reading on somebody's broker for ever.
        const bool retain = be_top(vm) >= 3 && be_tobool(vm, 3) != 0;
        if (leaf != nullptr && payload != nullptr) {
            sent = g_active.mqtt->publish(*g_active.scriptId, leaf, payload, retain);
        }
        g_active.publishes += 1;
    }
    be_pushbool(vm, sent ? 1 : 0);
    be_return(vm);
}

int b_mqtt_watch(bvm* vm) {
    bool watching = false;
    if (brokerReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        // Safe to call every frame, and scripts will: there is no "the broker
        // connected" callback for one to hook, so draw() is the only place a
        // watch can be asked for.
        watching = g_active.mqtt->watch(*g_active.scriptId, be_tostring(vm, 1));
    }
    be_pushbool(vm, watching ? 1 : 0);
    be_return(vm);
}

int b_mqtt_get(bvm* vm) {
    if (brokerReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        const std::string* value =
            g_active.mqtt->latest(*g_active.scriptId, be_tostring(vm, 1));
        if (value != nullptr) {
            be_pushstring(vm, value->c_str());
            be_return(vm);
        }
    }
    // nil, not "". See the note above: they are different answers.
    be_pushnil(vm);
    be_return(vm);
}

int b_mqtt_age_ms(bvm* vm) {
    std::int64_t age = -1;
    if (brokerReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        age = g_active.mqtt->ageMillis(*g_active.scriptId, be_tostring(vm, 1));
    }
    be_pushint(vm, static_cast<bint>(age));
    be_return(vm);
}

/* --- the network -----------------------------------------------------------
 *
 * Deliberately no call that fetches and returns.
 *
 * Such a call would have to block the thread that draws the panel, and
 * blueprint §16 forbids that outright - a thirty-second connect timeout would
 * be thirty seconds of frozen display. So a script says what it wants and how
 * often, something else fetches it, and the script reads whatever arrived
 * last.
 *
 * That is not a consolation prize. A script polling an API every five minutes
 * and drawing its last answer with a visible age is strictly better behaved
 * than the synchronous version: when the API goes down, the panel keeps its
 * last reading and shows that it is old, instead of hanging.
 */

bool networkReady() noexcept {
    return g_active.http != nullptr && g_active.scriptId != nullptr &&
           !g_active.scriptId->empty();
}

int b_http_known(bvm* vm) {
    be_pushbool(vm, (networkReady() && g_active.http->available()) ? 1 : 0);
    be_return(vm);
}

int b_http_follow(bvm* vm) {
    bool following = false;
    if (networkReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        // Seconds from the script, milliseconds underneath. Seconds because a
        // script author writing 300 means five minutes and writing 300000
        // means they have made a mistake the floor would silently absorb.
        std::uint32_t intervalMillis = 0;
        if (be_top(vm) >= 2) {
            const int seconds = argInt(vm, 2);
            if (seconds > 0) {
                intervalMillis = static_cast<std::uint32_t>(seconds) * 1000u;
            }
        }
        following = g_active.http->follow(*g_active.scriptId, be_tostring(vm, 1),
                                          intervalMillis);
    }
    be_pushbool(vm, following ? 1 : 0);
    be_return(vm);
}

int b_http_get(bvm* vm) {
    if (networkReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        const std::string* value =
            g_active.http->body(*g_active.scriptId, be_tostring(vm, 1));
        if (value != nullptr) {
            be_pushstring(vm, value->c_str());
            be_return(vm);
        }
    }
    // nil, not "". An endpoint that returned nothing and one that has never
    // answered are different states.
    be_pushnil(vm);
    be_return(vm);
}

int b_http_status(bvm* vm) {
    int status = 0;
    if (networkReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        status = g_active.http->status(*g_active.scriptId, be_tostring(vm, 1));
    }
    be_pushint(vm, status);
    be_return(vm);
}

int b_http_age_ms(bvm* vm) {
    std::int64_t age = -1;
    if (networkReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        age = g_active.http->ageMillis(*g_active.scriptId, be_tostring(vm, 1));
    }
    be_pushint(vm, static_cast<bint>(age));
    be_return(vm);
}

int b_http_error(bvm* vm) {
    if (networkReady() && be_top(vm) >= 1 && be_isstring(vm, 1)) {
        const std::string_view why =
            g_active.http->failure(*g_active.scriptId, be_tostring(vm, 1));
        if (!why.empty()) {
            // Copied through a std::string because the view is not
            // guaranteed to be null-terminated, and Berry wants a C string.
            const std::string held(why);
            be_pushstring(vm, held.c_str());
            be_return(vm);
        }
    }
    be_pushnil(vm);
    be_return(vm);
}

/* --- the microphone --------------------------------------------------------
 *
 * One amplitude, not a spectrum, and the builtins are shaped so a script
 * cannot pretend otherwise. The TC002 reports a single 16-bit level about
 * twenty times a second over its MCU link; anything here called `band()`
 * would be inventing the number it returned.
 *
 * Raw rather than normalised, because what counts as loud depends on the
 * room and the adapter cannot know that. A visualiser that wants a full-scale
 * bar keeps its own recent maximum - which is auto-gain, and belongs where
 * the history is.
 */

int b_mic_known(bvm* vm) {
    be_pushbool(vm, (g_active.microphone != nullptr &&
                     g_active.microphone->level().known)
                        ? 1
                        : 0);
    be_return(vm);
}

int b_mic_level(bvm* vm) {
    int amplitude = 0;
    if (g_active.microphone != nullptr) {
        const platform::SoundLevel level = g_active.microphone->level();
        // Zero when it cannot hear, and mic_known() is how a script tells
        // that from a silent room. Returning -1 here would be a second way
        // to say the same thing and a second way to get it wrong.
        amplitude = level.known ? level.amplitude : 0;
    }
    be_pushint(vm, amplitude);
    be_return(vm);
}

int b_now_ms(bvm* vm) {
    be_pushint(vm, static_cast<bint>(g_active.environment.monotonicMillis));
    be_return(vm);
}

/// Milliseconds since this app came on screen.
///
/// The right clock for an animation that should start from its beginning
/// every time the app appears, rather than joining part-way through.
int b_elapsed_ms(bvm* vm) {
    be_pushint(vm, static_cast<bint>(g_active.elapsedMillis));
    be_return(vm);
}

void registerBuiltins(bvm* vm) {
    be_regfunc(vm, "width", b_width);
    be_regfunc(vm, "height", b_height);
    be_regfunc(vm, "rgb", b_rgb);
    be_regfunc(vm, "clear", b_clear);
    be_regfunc(vm, "pixel", b_pixel);
    be_regfunc(vm, "line", b_line);
    be_regfunc(vm, "rect", b_rect);
    be_regfunc(vm, "rect_fill", b_rect_fill);
    be_regfunc(vm, "text", b_text);
    be_regfunc(vm, "text_width", b_text_width);
    be_regfunc(vm, "text_ink_width", b_text_width);
    be_regfunc(vm, "now_ms", b_now_ms);
    be_regfunc(vm, "elapsed_ms", b_elapsed_ms);

    be_regfunc(vm, "tone", b_tone);
    be_regfunc(vm, "sound", b_sound);
    be_regfunc(vm, "audio_known", b_audio_known);
    be_regfunc(vm, "volume", b_volume);

    be_regfunc(vm, "mqtt_known", b_mqtt_known);
    be_regfunc(vm, "mqtt_publish", b_mqtt_publish);
    be_regfunc(vm, "mqtt_watch", b_mqtt_watch);
    be_regfunc(vm, "mqtt_get", b_mqtt_get);
    be_regfunc(vm, "mqtt_age_ms", b_mqtt_age_ms);

    be_regfunc(vm, "http_known", b_http_known);
    be_regfunc(vm, "http_follow", b_http_follow);
    be_regfunc(vm, "http_get", b_http_get);
    be_regfunc(vm, "http_status", b_http_status);
    be_regfunc(vm, "http_age_ms", b_http_age_ms);
    be_regfunc(vm, "http_error", b_http_error);

    be_regfunc(vm, "mic_known", b_mic_known);
    be_regfunc(vm, "mic_level", b_mic_level);

    be_regfunc(vm, "hour", b_hour);
    be_regfunc(vm, "minute", b_minute);
    be_regfunc(vm, "second", b_second);
    be_regfunc(vm, "weekday", b_weekday);
    be_regfunc(vm, "day", b_day);
    be_regfunc(vm, "month", b_month);
    be_regfunc(vm, "year", b_year);
    be_regfunc(vm, "time_known", b_time_known);

    be_regfunc(vm, "battery", b_battery);
    be_regfunc(vm, "battery_known", b_battery_known);
    be_regfunc(vm, "charging", b_charging);

    be_regfunc(vm, "scroll_text", b_scroll_text);

    // The plain functions behind `store`. Named with a leading underscore
    // because the prelude wraps them and a script has no reason to call them
    // directly.
    be_regfunc(vm, "_store_get", b_store_get);
    be_regfunc(vm, "_store_set", b_store_set);
}

/// Berry that runs before any script.
///
/// Only what is genuinely easier to express in the language than in C. `store`
/// is a two-method object, which is five lines here against a page of
/// table-building from the C side, and costs about a hundred bytes in a VM
/// that costs four thousand.
constexpr const char* kPrelude = R"BERRY(
class _StippleStore
  def get(key, fallback)
    return _store_get(key, fallback)
  end
  def set(key, value)
    return _store_set(key, value)
  end
end
store = _StippleStore()
)BERRY";

/// The global the instance is stashed under.
///
/// Deliberately a name a script is unlikely to choose. It is not a security
/// boundary - a script could overwrite it - but overwriting it only breaks
/// that script, which is its own business.
constexpr const char* kInstance = "_stipple_app";

}  // namespace

struct ScriptHost::State {
    bvm* vm = nullptr;
};

ScriptHost::ScriptHost() : state_(new State()) {
    state_->vm = be_vm_new();
    if (state_->vm != nullptr) {
        be_set_obs_hook(state_->vm, observe);
        registerBuiltins(state_->vm);

        // The prelude is ours, so a failure here is a bug in this file rather
        // than in somebody's script - but it must not take the device with
        // it. A VM without `store` still runs every script that does not use
        // it, which is most of them.
        bvm* vm = state_->vm;
        const int top = be_top(vm);
        if (be_loadbuffer(vm, "prelude", kPrelude, std::strlen(kPrelude)) == 0) {
            be_pcall(vm, 0);
        }
        if (const int extra = be_top(vm) - top; extra > 0) {
            be_pop(vm, extra);
        }
    }
}

ScriptHost::~ScriptHost() {
    if (state_ != nullptr) {
        if (state_->vm != nullptr) {
            be_vm_delete(state_->vm);
        }
        delete state_;
    }
}

bool ScriptHost::load(std::string_view source, std::string& problem) {
    problem.clear();
    ready_ = false;

    if (state_ == nullptr || state_->vm == nullptr) {
        problem = "the script engine is unavailable";
        return false;
    }
    if (source.empty()) {
        problem = "the script is empty";
        return false;
    }
    if (source.size() > kMaxSourceBytes) {
        problem = "the script is too long";
        return false;
    }

    bvm* vm = state_->vm;

    // Compiling is bounded too. A pathological source can make a parser work
    // hard before it produces anything, and this one runs on the thread that
    // draws the panel.
    g_active = Active{};

    // Same stack discipline as draw(), and for the same reason - see the
    // comment there. Recorded depth in, restored depth out, on every path.
    const int topBefore = be_top(vm);
    bool ok = false;

    if (be_loadbuffer(vm, "app", source.data(), source.size()) != 0) {
        // Berry leaves the message on the stack, and it carries the line
        // number - which is the whole value of it to somebody in the editor.
        problem = be_isstring(vm, -1) ? be_tostring(vm, -1) : "the script would not compile";
    } else if (be_pcall(vm, 0) != 0) {
        problem = be_isstring(vm, -1) ? be_tostring(vm, -1) : "the script failed while loading";
    } else if (be_isnil(vm, -1)) {
        // A chunk that returns nothing is the commonest first mistake: the
        // author wrote the class and forgot to hand back an instance of it.
        problem = "the script did not return an app instance - end it with `return YourClass()`";
    } else {
        be_setglobal(vm, kInstance);
        ok = true;
    }

    if (const int extra = be_top(vm) - topBefore; extra > 0) {
        be_pop(vm, extra);
    }

    if (!ok) {
        return false;
    }

    ready_ = true;
    return true;
}

const std::vector<std::pair<std::string, ScriptHost::Stored>>&
ScriptHost::stored() const noexcept {
    return store_;
}

void ScriptHost::restoreStored(std::vector<std::pair<std::string, Stored>> values) {
    if (values.size() > kMaxStoreKeys) {
        values.resize(kMaxStoreKeys);
    }
    store_ = std::move(values);
}

std::uint32_t ScriptHost::durationMillis() {
    if (!ready_ || state_ == nullptr || state_->vm == nullptr) {
        return 0;
    }

    bvm* vm = state_->vm;
    const int topBefore = be_top(vm);
    std::uint32_t millis = 0;

    g_active.environment = environment_;
    g_active.store = &store_;
    g_active.audio = audio_;
    g_active.tones = 0;
    g_active.mqtt = mqtt_;
    g_active.scriptId = &scriptId_;
    g_active.publishes = 0;
    g_active.http = http_;
    g_active.microphone = microphone_;
    g_active.heartbeats = 0;
    g_active.overBudget = false;

    if (be_getglobal(vm, kInstance)) {
        if (be_getmethod(vm, -1, "duration")) {
            be_pushvalue(vm, -2);
            if (be_pcall(vm, 1) == 0 && be_isint(vm, -1)) {
                const bint value = be_toint(vm, -1);
                // Clamped rather than trusted. A script asking for a week on
                // screen has made a mistake, and honouring it would look
                // exactly like the carousel having stopped.
                constexpr bint kCeiling = 10 * 60 * 1000;
                millis = static_cast<std::uint32_t>(
                    value < 0 ? 0 : (value > kCeiling ? kCeiling : value));
            }
        }
    }

    if (const int extra = be_top(vm) - topBefore; extra > 0) {
        be_pop(vm, extra);
    }
    g_active.store = nullptr;
    g_active.audio = nullptr;
    g_active.mqtt = nullptr;
    g_active.http = nullptr;
    g_active.microphone = nullptr;
    g_active.scriptId = nullptr;
    return millis;
}

void ScriptHost::setAudio(platform::IAudioOutput* audio) noexcept {
    audio_ = audio;
}

bool ScriptHost::setStored(std::string_view key, Stored value) {
    if (key.empty() || key.size() > kMaxStoreKeyBytes) {
        return false;
    }
    if (value.kind == Stored::Kind::Text && value.text.size() > kMaxStoreTextBytes) {
        value.text.resize(kMaxStoreTextBytes);
    }

    for (auto& entry : store_) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return true;
        }
    }
    if (store_.size() >= kMaxStoreKeys) {
        return false;
    }
    store_.emplace_back(std::string(key), std::move(value));
    return true;
}

void ScriptHost::setMqtt(IScriptMqtt* mqtt, std::string_view scriptId) {
    mqtt_ = mqtt;
    scriptId_.assign(scriptId);
}

void ScriptHost::setHttp(IScriptHttp* http, std::string_view scriptId) {
    http_ = http;
    scriptId_.assign(scriptId);
}

void ScriptHost::setMicrophone(platform::IMicrophone* microphone) noexcept {
    microphone_ = microphone;
}

void ScriptHost::setEnvironment(const ScriptEnvironment& environment) noexcept {
    environment_ = environment;
}

std::size_t ScriptHost::memoryBytes() const noexcept {
    if (state_ == nullptr || state_->vm == nullptr) {
        return 0;
    }
    return be_gc_memcount(state_->vm);
}

std::size_t ScriptHost::collectGarbage() noexcept {
    if (state_ == nullptr || state_->vm == nullptr) {
        return 0;
    }
    be_gc_collect(state_->vm);
    return be_gc_memcount(state_->vm);
}

ScriptHost::EventResult ScriptHost::invoke(const char* method, const char* argument,
                                           std::string& problem) {
    // Every call into a script goes through here, so the stack discipline
    // lives in one place.
    //
    // That is not tidiness. The first version of draw() popped a count one
    // short of what it pushed and leaked a single 16-byte slot per frame -
    // invisible in any short test, fatal after about two minutes on screen
    // once BE_STACK_TOTAL_MAX ran out. Adding on_button by copying that
    // sequence would have copied the bug with it. So the depth is recorded
    // and restored, which is right whatever the call leaves behind, including
    // on the error paths where the exception value's position is least
    // obvious.
    bvm* vm = state_->vm;
    const int topBefore = be_top(vm);
    EventResult result = EventResult::NotDefined;

    if (be_getglobal(vm, kInstance)) {
        if (be_getmethod(vm, -1, method)) {
            // The instance is the receiver, so it moves above the method.
            be_pushvalue(vm, -2);
            int argc = 1;
            if (argument != nullptr) {
                be_pushstring(vm, argument);
                ++argc;
            }
            if (be_pcall(vm, argc) == 0) {
                result = EventResult::Handled;
            } else {
                problem = be_isstring(vm, -1) ? be_tostring(vm, -1)
                                              : "the script failed while running";
                result = EventResult::Failed;
            }
        }
        // Otherwise NotDefined, which is not an error. An optional callback
        // the script did not write must let the press fall through rather
        // than swallowing it.
    } else {
        problem = "the script instance is gone";
        result = EventResult::Failed;
    }

    if (const int extra = be_top(vm) - topBefore; extra > 0) {
        be_pop(vm, extra);
    }
    return result;
}

bool ScriptHost::draw(Canvas& canvas, std::uint64_t elapsedMillis, std::string& problem) {
    problem.clear();
    if (!ready_ || state_ == nullptr || state_->vm == nullptr) {
        problem = "no script is loaded";
        return false;
    }

    g_active.canvas = &canvas;
    g_active.elapsedMillis = elapsedMillis;
    g_active.environment = environment_;
    g_active.store = &store_;
    g_active.audio = audio_;
    g_active.tones = 0;
    g_active.mqtt = mqtt_;
    g_active.scriptId = &scriptId_;
    g_active.publishes = 0;
    g_active.http = http_;
    g_active.microphone = microphone_;
    g_active.heartbeats = 0;
    g_active.overBudget = false;

    const EventResult result = invoke("draw", nullptr, problem);

    lastInstructions_ = g_active.heartbeats * 65536u;
    if (g_active.overBudget && problem.empty()) {
        problem = "the script ran too long for one frame";
    }
    if (result == EventResult::NotDefined) {
        problem = "the script has no draw() method";
    }
    g_active.canvas = nullptr;
    g_active.store = nullptr;
    g_active.audio = nullptr;
    g_active.mqtt = nullptr;
    g_active.http = nullptr;
    g_active.microphone = nullptr;
    g_active.scriptId = nullptr;

    if (result != EventResult::Handled) {
        // Disabled rather than retried. A script that throws thirty times a
        // second fills the log and starves everything else of time, and the
        // author needs to see the first error rather than the ten thousandth.
        ready_ = false;
        return false;
    }
    return true;
}

ScriptHost::EventResult ScriptHost::button(std::string_view name, std::string& problem) {
    problem.clear();
    if (!ready_ || state_ == nullptr || state_->vm == nullptr) {
        return EventResult::NotDefined;
    }

    // No canvas. A button arrives between frames, not during one, and the
    // drawing builtins check for a canvas before touching anything - so a
    // handler that tries to draw quietly does nothing rather than writing
    // into whatever the last frame left behind.
    g_active.canvas = nullptr;
    g_active.environment = environment_;
    g_active.store = &store_;
    g_active.audio = audio_;
    g_active.tones = 0;
    g_active.mqtt = mqtt_;
    g_active.scriptId = &scriptId_;
    g_active.publishes = 0;
    g_active.http = http_;
    g_active.microphone = microphone_;
    g_active.heartbeats = 0;
    g_active.overBudget = false;

    const std::string held(name);
    const EventResult result = invoke("on_button", held.c_str(), problem);

    lastInstructions_ = g_active.heartbeats * 65536u;
    if (g_active.overBudget && problem.empty()) {
        problem = "the button handler ran too long";
    }
    g_active.store = nullptr;
    g_active.audio = nullptr;
    g_active.mqtt = nullptr;
    g_active.http = nullptr;
    g_active.microphone = nullptr;
    g_active.scriptId = nullptr;

    if (result == EventResult::Failed) {
        // A handler that loops for ever is exactly as bad as a draw() that
        // does, and gets the same answer.
        ready_ = false;
    }
    return result;
}

}  // namespace script
}  // namespace stipple
