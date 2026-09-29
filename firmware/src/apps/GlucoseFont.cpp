// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseFont.h"

#include "stipple/graphics/Canvas.h"

namespace stipple {
namespace apps {
namespace glucose {
namespace {

char upperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

/// A glyph with no ink (space) carries -1 bounds in the generated table.
bool blank(const fontdata::Glyph& glyph) noexcept {
    return glyph.c0 < 0 || glyph.c1 < glyph.c0;
}

/// First and last inked row across the drawable glyphs of `text`. False when
/// nothing in it has ink.
bool inkRows(const fontdata::Font& font, std::string_view text, int& first, int& last) noexcept {
    bool any = false;
    for (const char c : text) {
        const fontdata::Glyph* glyph = findGlyph(font, c);
        if (glyph == nullptr || blank(*glyph)) {
            continue;
        }
        if (!any || glyph->r0 < first) {
            first = glyph->r0;
        }
        if (!any || glyph->r1 > last) {
            last = glyph->r1;
        }
        any = true;
    }
    return any;
}

bool lit(const std::uint16_t row, int width, int column) noexcept {
    return ((row >> (width - 1 - column)) & 1u) != 0u;
}

}  // namespace

const fontdata::Glyph* findGlyph(const fontdata::Font& font, char c) noexcept {
    const char wanted = upperAscii(c);
    for (int i = 0; i < font.count; ++i) {
        if (font.glyphs[i].ch == wanted) {
            return &font.glyphs[i];
        }
    }
    return nullptr;
}

int textWidth(const fontdata::Font& font, std::string_view text) noexcept {
    int total = 0;
    for (const char c : text) {
        const fontdata::Glyph* glyph = findGlyph(font, c);
        if (glyph == nullptr) {
            continue;
        }
        total += (blank(*glyph) ? kSpaceAdvance : glyph->c1 - glyph->c0 + 1) + kTracking;
    }
    return total > kTracking ? total - kTracking : 0;
}

int textHeight(const fontdata::Font& font, std::string_view text) noexcept {
    int first = 0;
    int last = 0;
    return inkRows(font, text, first, last) ? last - first + 1 : 0;
}

int drawText(Canvas& canvas, int x, int y, std::string_view text, const fontdata::Font& font,
             Rgb color) noexcept {
    int top = 0;
    int unused = 0;
    if (!inkRows(font, text, top, unused)) {
        top = 0;
    }
    for (const char c : text) {
        const fontdata::Glyph* glyph = findGlyph(font, c);
        if (glyph == nullptr) {
            continue;
        }
        if (blank(*glyph)) {
            x += kSpaceAdvance + kTracking;
            continue;
        }
        for (int row = top; row < font.height; ++row) {
            for (int column = glyph->c0; column <= glyph->c1; ++column) {
                if (lit(glyph->rows[row], font.width, column)) {
                    canvas.pixel(x + column - glyph->c0, y + (row - top), color);
                }
            }
        }
        x += (glyph->c1 - glyph->c0 + 1) + kTracking;
    }
    return x - kTracking;
}

void drawTextRight(Canvas& canvas, int xRight, int y, std::string_view text,
                   const fontdata::Font& font, Rgb color) noexcept {
    drawText(canvas, xRight - textWidth(font, text) + 1, y, text, font, color);
}

int drawTiny(Canvas& canvas, int x, int y, std::string_view text, Rgb color) noexcept {
    const fontdata::Font& font = fontdata::kTiny;
    for (const char c : text) {
        const fontdata::Glyph* glyph = findGlyph(font, c);
        if (glyph == nullptr) {
            continue;
        }
        const int width = glyph->c1 + 1;
        for (int row = 0; row < font.height; ++row) {
            for (int column = 0; column < width; ++column) {
                if (lit(glyph->rows[row], width, column)) {
                    canvas.pixel(x + column, y + row, color);
                }
            }
        }
        x += width + 1;
    }
    return x - 2;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
