// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stipple {
namespace script {

/// One setting a script has asked the device to collect on its behalf.
///
/// Declared in a comment at the top of the script:
///
///     # @config chan text "Channel ID" default="UC..." maxlen=32 help="..."
///
/// A comment, so the file stays one thing you can download and paste into
/// the editor without stripping a header off it - the same reason the shop's
/// `# name:` and `# tags:` live there.
///
/// **The point is that nobody edits Berry to change a channel ID.** Before
/// this, "set your own channel" meant opening the source, finding a string
/// literal and hoping. Now the web UI renders a labelled field and the value
/// lands in the script's own store, which was already persisted.
struct Setting {
    enum class Type : std::uint8_t {
        Text,
        Number,
        Boolean,
    };

    /// What the script reads with `store.get(key, fallback)`. The declaration
    /// and the read use the same key on purpose: there is no second namespace
    /// to keep in step, and a script whose device is too old to know about
    /// `@config` still works, because `store.get` falls back on its own.
    std::string key;
    Type type = Type::Text;

    /// Shown beside the field. Required - an unlabelled box is a box nobody
    /// can fill in.
    std::string label;

    /// Shown under it. Optional, and worth writing: "the UC... part of the
    /// URL, not the @handle" is the difference between a working app and a
    /// confused person.
    std::string help;

    /// What the field shows when nothing has been set. Always text here even
    /// for numbers and booleans, because that is what the UI renders and
    /// what the store round-trips.
    std::string fallback;

    /// Text only. Zero means the type's own limit applies.
    int maxLength = 0;

    /// Number only.
    long long minimum = 0;
    long long maximum = 0;
    bool bounded = false;
};

/// Settings one script may declare.
///
/// Eight. A script needing more than eight fields is an application with a
/// configuration file, and this is a panel 52 pixels wide.
inline constexpr std::size_t kMaxSettings = 8;

/// Longest key, label and help text kept.
inline constexpr std::size_t kMaxSettingKeyBytes = 24;
inline constexpr std::size_t kMaxSettingTextBytes = 96;

const char* settingTypeName(Setting::Type type) noexcept;

/// Pull every `# @config` declaration out of a script's source.
///
/// Stops at the first line that is neither a comment nor blank, for the same
/// reason the shop's header parser does: a `@config` written halfway down the
/// file is a comment about the code there, and honouring it would be a guess.
///
/// Malformed declarations are skipped rather than failing the script. A typo
/// in a settings line should cost that setting, not the app - the source
/// still compiles and still runs, and the editor shows what was understood.
std::vector<Setting> parseSettings(std::string_view source);

/// How much of the panel's input a script has asked for (ADR 0024).
enum class InputMode : std::uint8_t {
    /// The default, and what every script written before this got: the action
    /// press reaches `on_button("select")` and nothing else does.
    ActionOnly,

    /// Declared as `# @input exclusive`. The - and + buttons, the knob press
    /// and both knob detents all reach `on_button`, by the same names
    /// `/api/v1/input` uses: minus, plus, select, left, right.
    ///
    /// **The middle button is never included, and neither is a held knob.**
    /// Those are how somebody leaves, and a script that could take them would
    /// be a script you could not leave - which was the whole objection to
    /// offering the knob at all. Keeping one control reserved answers it
    /// without withholding the rest.
    Exclusive,
};

/// Read the `# @input` declaration out of a script's header.
///
/// Same rules as parseSettings: header comments only, stops at the first line
/// of code, and anything unrecognised is ignored rather than failing the
/// script. A script that asks for an input mode this firmware has never heard
/// of gets the default and still runs, which is what lets the set grow without
/// stranding anyone on an older device.
InputMode parseInputMode(std::string_view source);

}  // namespace script
}  // namespace stipple
