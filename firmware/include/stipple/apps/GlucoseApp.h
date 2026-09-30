// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "stipple/apps/GlucoseModel.h"
#include "stipple/core/Rgb.h"

namespace stipple {

class Canvas;

namespace apps {

/// Glucose faces.
///
/// A port of nightscout-pixbar's `faces.py`, which is the pixel-exact reference:
/// every face here reproduces that renderer's golden corpus byte for byte, and
/// the test suite holds it to that. Colour is not decoration on these faces -
/// the value's colour IS the glucose state (green / yellow / red, gray when
/// stale), so everything else stays white or dim and never competes with it.
enum class GlucoseFace : std::uint8_t {
    /// One number, as big as the panel allows, plus direction. Nothing else.
    Hero,
    /// Hero value, direction, and the small facts that still fit beside them.
    HeroDelta,
    /// Hero value on the left, three hours of history on the right.
    HeroGraph,
    /// Bedside: time and value side by side, both at one readable size.
    Clock,
    /// The panel is history; the value rides on the right over a cleared box.
    BigGraph,
    /// Not a blank value - a statement, with how long it has been that way.
    NoData,
};

inline constexpr int kGlucoseFaceCount = 6;

/// The faces a person may choose. NoData is what a stale reading is drawn
/// as, not a choice, so the knob never lands on it.
inline constexpr int kGlucoseSelectableFaceCount = 5;

GlucoseFace glucoseFaceFromName(std::string_view name) noexcept;
const char* glucoseFaceName(GlucoseFace face) noexcept;
GlucoseFace glucoseFaceAt(int index) noexcept;

/// The next (+1) or previous (-1) selectable face, wrapping. Any other step
/// counts as its sign; NoData steps to Hero.
GlucoseFace glucoseFaceStep(GlucoseFace face, int direction) noexcept;

/// FastLED `scale8` with `FASTLED_SCALE8_FIXED`: `(c * (factor + 1)) >> 8`.
///
/// Not `stipple::scale`, which divides by 255. The reference corpus and its
/// night-legibility audit are valid for this rule and no other, so the faces
/// dim their own accents with it and the tests judge low brightness with it.
constexpr Rgb scale8(Rgb color, std::uint8_t factor) noexcept {
    const unsigned f = static_cast<unsigned>(factor) + 1u;
    return Rgb{static_cast<std::uint8_t>((static_cast<unsigned>(color.r) * f) >> 8),
               static_cast<std::uint8_t>((static_cast<unsigned>(color.g) * f) >> 8),
               static_cast<std::uint8_t>((static_cast<unsigned>(color.b) * f) >> 8)};
}

/// The colour a value's band is drawn in.
Rgb glucoseBandColor(int sgv) noexcept;

/// The colour a reading is drawn in: its band, or gray once stale.
Rgb glucoseReadingColor(const glucose::Reading& reading) noexcept;

/// Draw `reading` in `face`. Pure: the same reading always draws the same frame.
void renderGlucose(Canvas& canvas, const glucose::Reading& reading, GlucoseFace face);

/// True when a face would differ between these two moments. Nothing on a
/// glucose face moves faster than the minute (age pips, the stale rule, the
/// clock), so a static reading is redrawn once a minute, not once a frame.
bool glucoseChanged(std::uint64_t previousMillis, std::uint64_t nowMillis) noexcept;

}  // namespace apps
}  // namespace stipple
