// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

#include "stipple/apps/GlucoseFontData.h"
#include "stipple/core/Rgb.h"

namespace stipple {

class Canvas;

namespace apps {
namespace glucose {

/// The glucose faces' own text engine.
///
/// Stipple's text engine tops out at eight glyph rows (`text::kMaxGlyphRows`);
/// the hero digit on a glucose face is fifteen. So the glucose app carries the
/// Spleen glyphs the reference renderer bakes and draws them the way it does:
/// every glyph trimmed to its ink, one pixel of tracking between glyphs, a blank
/// glyph advancing three, and the whole string sat so its first inked row lands
/// on the y it asked for. That is what makes the frames byte-identical to the
/// reference corpus, which is the point of the exercise.

inline constexpr int kTracking = 1;
inline constexpr int kSpaceAdvance = 3;

/// Lookup is case-insensitive for ASCII letters; unknown characters are skipped.
const fontdata::Glyph* findGlyph(const fontdata::Font& font, char c) noexcept;

/// Advance width of `text` drawn in `font`, ink-trimmed. Zero for no ink.
int textWidth(const fontdata::Font& font, std::string_view text) noexcept;

/// Ink height of `text` in `font`: last inked row minus first, over all glyphs.
int textHeight(const fontdata::Font& font, std::string_view text) noexcept;

/// Draw with the ink box starting at (x, y). Returns the first column after the
/// last glyph's ink (the caller adds its own gap).
int drawText(Canvas& canvas, int x, int y, std::string_view text, const fontdata::Font& font,
             Rgb color) noexcept;

/// Draw so the ink ends at column `xRight` inclusive.
void drawTextRight(Canvas& canvas, int xRight, int y, std::string_view text,
                   const fontdata::Font& font, Rgb color) noexcept;

/// The 3x5 fallback, top-left anchored, untrimmed. Returns the x of the last lit
/// column.
int drawTiny(Canvas& canvas, int x, int y, std::string_view text, Rgb color) noexcept;

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
