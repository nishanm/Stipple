// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseApp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "stipple/apps/GlucoseFont.h"
#include "stipple/graphics/Canvas.h"
#include "stipple/graphics/Framebuffer.h"

namespace stipple {
namespace apps {
namespace {

using glucose::Reading;
using glucose::Trend;
using glucose::fontdata::kHero;
using glucose::fontdata::kMid;
using glucose::fontdata::kSmall;

// The TC001 firmware's RGB565 constants expanded to RGB888, as the reference
// renderer does it. Gray is deliberately not (164, 164, 164): 0xA514 expands to
// 164, 161, 164 and the goldens carry that.
constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kYellow{255, 255, 0};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGray{164, 161, 164};
constexpr Rgb kWhite{255, 255, 255};

constexpr int kPanelWidth = Framebuffer::kWidth;
constexpr int kPanelHeight = Framebuffer::kHeight;

/// Left/right margin; nothing but a full-bleed graph touches the edge columns.
constexpr int kPad = 1;

constexpr int kSpanMinutes = 180;

/// A filled rectangle drawn pixel by pixel. Canvas::fillRect would do, but this
/// matches the reference's `Panel.rect` exactly, including how a zero or
/// negative extent draws nothing, and that literalness is what the goldens buy.
void fill(Canvas& canvas, int x, int y, int width, int height, Rgb color) noexcept {
    for (int dy = 0; dy < height; ++dy) {
        for (int dx = 0; dx < width; ++dx) {
            canvas.pixel(x + dx, y + dy, color);
        }
    }
}

/// Python's `int(round(v))`: round half to even on the double itself. The
/// reference corpus really does land on exact halves (a 78-minute-old point on
/// a 16-column graph is 6.5), so half-away rounding would move pixels.
int roundHalfEven(double value) noexcept {
    return static_cast<int>(std::nearbyint(value));
}

// --- trend mark ---------------------------------------------------------------

int slopeOf(Trend trend) noexcept {
    switch (trend) {
        case Trend::DoubleUp:
        case Trend::SingleUp: return 2;
        case Trend::FortyFiveUp: return 1;
        case Trend::FortyFiveDown: return -1;
        case Trend::SingleDown:
        case Trend::DoubleDown: return -2;
        case Trend::Flat:
        case Trend::None: break;
    }
    return 0;
}

/// Up/down: stacked chevrons on a stem. Flat: a bar with a head. 45s: a
/// diagonal. Doubles get two chevrons. None: a question mark that fits the box.
/// Drawn, not bitmapped, so it scales with whatever it sits beside.
void trendMark(Canvas& canvas, int x, int y, Trend trend, Rgb color, int w, int h) noexcept {
    if (trend == Trend::None) {
        // Largest glyph that fits the height given; below every Spleen size
        // (the clock face offers 6 rows) the 3x5 font is the honest fallback.
        for (const glucose::fontdata::Font* font : {&kMid, &kSmall}) {
            const int glyphHeight = glucose::textHeight(*font, "?");
            if (glyphHeight <= h) {
                glucose::drawText(canvas, x, y + (h - glyphHeight) / 2, "?", *font, color);
                return;
            }
        }
        glucose::drawTiny(canvas, x, y + std::max(0, (h - kSmall.height) / 2), "?", color);
        return;
    }

    const int slope = slopeOf(trend);
    const int mid = y + h / 2;

    if (slope == 0) {
        fill(canvas, x, mid - 1, w - 3, 2, color);  // shaft
        for (int i = 0; i < 3; ++i) {               // head, 5 -> 1 tall
            fill(canvas, x + w - 3 + i, mid - 2 + i, 1, 5 - 2 * i, color);
        }
        return;
    }

    if (slope == 1 || slope == -1) {
        // 2x2 blocks along the diagonal. The run is one shorter than the box in
        // both axes or it spills a corner.
        const int span = std::min(w - 1, h - 1);
        for (int i = 0; i < span; ++i) {
            const int yy = slope > 0 ? y + (h - 2 - i) : y + i;
            fill(canvas, x + i, yy, 2, 2, color);
        }
        const int headY = slope > 0 ? y : y + h - 2;
        fill(canvas, x + w - 6, headY, 6, 2, color);
        // The stroke stretches to meet the last block rather than floating at a
        // fixed length - a box far taller than wide caps span at w - 1 and a
        // six-pixel stroke would stop rows short of it.
        const int lastYY = slope > 0 ? y + (h - 1 - span) : y + span - 1;
        const int reach = std::abs(lastYY - headY) + 2;
        const int strokeLength = std::max(6, reach);
        const int strokeY = slope > 0 ? headY : headY - strokeLength + 2;
        fill(canvas, x + w - 2, strokeY, 2, strokeLength, color);
        return;
    }

    const int count = (trend == Trend::DoubleUp || trend == Trend::DoubleDown) ? 2 : 1;
    const int stem = x + w / 2 - 1;
    fill(canvas, stem, y, 2, h, color);
    const int reach = w / 2;
    for (int n = 0; n < count; ++n) {
        const int base = slope > 0 ? y + n * (h / 2 - 1) : y + h - 2 - n * (h / 2 - 1);
        for (int i = 0; i < reach; ++i) {
            const int yy = slope > 0 ? base + i : base - i;
            fill(canvas, stem - i, yy, 2, 1, color);
            fill(canvas, stem + i, yy, 2, 1, color);
        }
    }
}

// --- graph --------------------------------------------------------------------

struct BandRows {
    int lo;
    int hi;
    int first;
    int last;
};

/// Fixed row ranges per band: the reader sees "in the red zone" without
/// reading an axis. Indexed by zone(): 0 urgent-high, 1 warning-high,
/// 2 normal, 3 warning-low, 4 urgent-low.
struct Bands {
    BandRows rows[5];
};

Bands bandsFor(int top, int bottom) noexcept {
    const int h = bottom - top + 1;
    const int urgent = h < 10 ? 1 : 2;
    const int warn = h < 8 ? 1 : 2;
    Bands bands;
    bands.rows[0] = {glucose::kUrgentHigh, 1000000, top, top + urgent - 1};
    bands.rows[1] = {glucose::kWarnHigh, glucose::kUrgentHigh, top + urgent, top + urgent + warn - 1};
    bands.rows[2] = {glucose::kWarnLow, glucose::kWarnHigh, top + urgent + warn, bottom - urgent - warn};
    bands.rows[3] = {glucose::kUrgentLow, glucose::kWarnLow, bottom - urgent - warn + 1, bottom - urgent};
    bands.rows[4] = {0, glucose::kUrgentLow, bottom - urgent + 1, bottom};
    return bands;
}

int zone(int sgv) noexcept {
    switch (glucose::band(sgv)) {
        case glucose::Band::Normal: return 2;
        case glucose::Band::Warning: return sgv > glucose::kWarnHigh ? 1 : 3;
        case glucose::Band::Urgent: break;
    }
    return sgv > glucose::kWarnHigh ? 0 : 4;
}

int rowFor(int sgv, const Bands& bands) noexcept {
    const BandRows& b = bands.rows[zone(sgv)];
    double frac = static_cast<double>(sgv - b.lo) / static_cast<double>(std::max(1, b.hi - b.lo));
    frac = std::min(1.0, std::max(0.0, frac));
    // Two statements on purpose: `last - frac * span` in one expression is a
    // fused multiply-add candidate under clang's default contraction, and a
    // fused result can differ from the reference by one unit in the last place
    // - exactly where a half-to-even tie is decided.
    const double scaled = frac * static_cast<double>(b.last - b.first);
    const double row = static_cast<double>(b.last) - scaled;
    return roundHalfEven(row);
}

/// Plotted against TIME, not array position: a source that repeats or drops
/// readings would otherwise stretch and squeeze the timeline.
void drawGraph(Canvas& canvas, int x, int y, int w, int h, const Reading& reading) noexcept {
    const int top = y;
    const int bottom = y + h - 1;
    const Bands bands = bandsFor(top, bottom);

    // The in-range band gets a dim floor and ceiling so the trace has a frame of
    // reference. Marking the urgent rows instead would paint red across a
    // perfectly healthy panel.
    const Rgb rail = scale8(kWhite, 34);
    for (int gx = x; gx < x + w; gx += 3) {
        canvas.pixel(gx, bands.rows[2].first, rail);
        canvas.pixel(gx, bands.rows[2].last, rail);
    }

    for (int i = 0; i < reading.historyCount; ++i) {
        const glucose::Sample& sample = reading.history[static_cast<std::size_t>(i)];
        const double ageMinutes = static_cast<double>(reading.now - sample.epoch) / 60.0;
        if (ageMinutes < 0 || ageMinutes > kSpanMinutes) {
            continue;
        }
        const double position = ageMinutes / kSpanMinutes * (w - 1);
        const int column = x + w - 1 - roundHalfEven(position);
        canvas.pixel(column, rowFor(sample.sgv, bands), glucoseBandColor(sample.sgv));
    }
}

// --- small facts --------------------------------------------------------------

/// How stale, as pips that go out one by one. Four steps is all a glance can carry.
void agePips(Canvas& canvas, int x, int y, int minutesAgo, Rgb color) noexcept {
    constexpr int kCount = 4;
    constexpr int kGap = 2;
    const int litCount = kCount - std::min(kCount, minutesAgo / (glucose::kStaleMinutes / kCount));
    for (int i = 0; i < kCount; ++i) {
        canvas.pixel(x + i * (1 + kGap), y, i < litCount ? color : scale8(color, 14));
    }
}

void timeText(const Reading& reading, char* out, std::size_t size) noexcept {
    if (!reading.timeKnown) {
        std::snprintf(out, size, "--:--");
        return;
    }
    std::snprintf(out, size, "%02d:%02d", reading.hour, reading.minute);
}

/// Draw the value as large as it goes, vertically centred, and report the
/// first free column. 48 and 287 are different widths, so nothing downstream
/// may assume a fixed one.
int heroValue(Canvas& canvas, const Reading& reading, Rgb color) noexcept {
    char text[8];
    reading.valueText(text, sizeof text);
    const int h = glucose::textHeight(kHero, text);
    const int y = (kPanelHeight - h) / 2;
    const int end = glucose::drawText(canvas, kPad, y, text, kHero, color);
    return end + 1;
}

// --- faces --------------------------------------------------------------------

void faceHero(Canvas& canvas, const Reading& r) noexcept {
    const Rgb c = glucoseReadingColor(r);
    const int free = heroValue(canvas, r, c);
    constexpr int kMarkWidth = 9;
    trendMark(canvas, std::max(free + 2, kPanelWidth - kMarkWidth - kPad), 1, r.trendShown(), c,
              kMarkWidth, 15);
}

void faceHeroDelta(Canvas& canvas, const Reading& r) noexcept {
    const Rgb c = glucoseReadingColor(r);
    const int free = heroValue(canvas, r, c) + 2;
    constexpr int kMarkWidth = 7;
    trendMark(canvas, free, 1, r.trendShown(), c, kMarkWidth, 15);
    const int right = free + kMarkWidth + 2;
    char delta[8];
    r.deltaText(delta, sizeof delta);
    // Only claim the right column when the value actually left one - a 3-digit
    // reading plus a mark is already 48 of the 52 columns.
    const int last = kPanelWidth - 1 - kPad;
    if (delta[0] != '\0' && right + glucose::textWidth(kSmall, delta) - 1 <= last) {
        glucose::drawText(canvas, right, 1, delta, kSmall, scale8(kWhite, 150));
        agePips(canvas, right, 13, r.minutesAgo, c);
    } else {
        agePips(canvas, last - 9, 14, r.minutesAgo, c);
    }
}

void faceHeroGraph(Canvas& canvas, const Reading& r) noexcept {
    const Rgb c = glucoseReadingColor(r);
    const int free = heroValue(canvas, r, c);
    const int gx = std::max(free + 2, 31);
    drawGraph(canvas, gx, 0, kPanelWidth - gx, kPanelHeight, r);
}

void faceClock(Canvas& canvas, const Reading& r) noexcept {
    const Rgb c = glucoseReadingColor(r);
    char time[8];
    timeText(r, time, sizeof time);
    char value[8];
    r.valueText(value, sizeof value);
    // Time is secondary, so it takes the small font; the value stays MID and
    // right-aligned. 23 + 18 + margins fits 52 with room to breathe.
    glucose::drawText(canvas, kPad, 1, time, kSmall, scale8(kWhite, 110));
    glucose::drawTextRight(canvas, kPanelWidth - 1 - kPad, 3, value, kMid, c);
    trendMark(canvas, kPad, 10, r.trendShown(), c, 7, 6);
    agePips(canvas, 12, 13, r.minutesAgo, c);
}

void faceBigGraph(Canvas& canvas, const Reading& r) noexcept {
    const Rgb c = glucoseReadingColor(r);
    drawGraph(canvas, 0, 0, kPanelWidth, kPanelHeight, r);
    char label[8];
    r.valueText(label, sizeof label);
    const int w = glucose::textWidth(kMid, label);
    fill(canvas, kPanelWidth - w - 3, 2, w + 3, 12, kBlack);
    glucose::drawTextRight(canvas, kPanelWidth - 1, 3, label, kMid, c);
}

void faceNoData(Canvas& canvas, const Reading& r) noexcept {
    // The "?" is the STALE colour, not red: red is a glucose state on every
    // face, and "no data" is not a glucose state.
    glucose::drawText(canvas, kPad, (kPanelHeight - glucose::textHeight(kHero, "?")) / 2, "?",
                      kHero, kGray);
    char label[8];
    std::snprintf(label, sizeof label, "%dM", r.minutesAgo);
    glucose::drawTextRight(canvas, kPanelWidth - 1 - kPad,
                           (kPanelHeight - glucose::textHeight(kMid, label)) / 2, label, kMid,
                           scale8(kWhite, 150));
}

}  // namespace

// --- faces registry -----------------------------------------------------------

GlucoseFace glucoseFaceFromName(std::string_view name) noexcept {
    if (name == "hero-delta") return GlucoseFace::HeroDelta;
    if (name == "hero-graph") return GlucoseFace::HeroGraph;
    if (name == "clock") return GlucoseFace::Clock;
    if (name == "big-graph") return GlucoseFace::BigGraph;
    if (name == "no-data") return GlucoseFace::NoData;
    return GlucoseFace::Hero;
}

const char* glucoseFaceName(GlucoseFace face) noexcept {
    switch (face) {
        case GlucoseFace::HeroDelta: return "hero-delta";
        case GlucoseFace::HeroGraph: return "hero-graph";
        case GlucoseFace::Clock: return "clock";
        case GlucoseFace::BigGraph: return "big-graph";
        case GlucoseFace::NoData: return "no-data";
        case GlucoseFace::Hero: break;
    }
    return "hero";
}

GlucoseFace glucoseFaceAt(int index) noexcept {
    switch (index) {
        case 1: return GlucoseFace::HeroDelta;
        case 2: return GlucoseFace::HeroGraph;
        case 3: return GlucoseFace::Clock;
        case 4: return GlucoseFace::BigGraph;
        case 5: return GlucoseFace::NoData;
        default: return GlucoseFace::Hero;
    }
}

GlucoseFace glucoseFaceStep(GlucoseFace face, int direction) noexcept {
    const int index = static_cast<int>(face);
    if (index >= kGlucoseSelectableFaceCount) {
        return GlucoseFace::Hero;
    }
    const int step = direction < 0 ? -1 : 1;
    const int next = (index + step + kGlucoseSelectableFaceCount) % kGlucoseSelectableFaceCount;
    return glucoseFaceAt(next);
}

Rgb glucoseBandColor(int sgv) noexcept {
    switch (glucose::band(sgv)) {
        case glucose::Band::Warning: return kYellow;
        case glucose::Band::Urgent: return kRed;
        case glucose::Band::Normal: break;
    }
    return kGreen;
}

Rgb glucoseReadingColor(const Reading& reading) noexcept {
    return reading.stale() ? kGray : glucoseBandColor(reading.sgv);
}

void renderGlucose(Canvas& canvas, const Reading& reading, GlucoseFace face) {
    switch (face) {
        case GlucoseFace::Hero: faceHero(canvas, reading); return;
        case GlucoseFace::HeroDelta: faceHeroDelta(canvas, reading); return;
        case GlucoseFace::HeroGraph: faceHeroGraph(canvas, reading); return;
        case GlucoseFace::Clock: faceClock(canvas, reading); return;
        case GlucoseFace::BigGraph: faceBigGraph(canvas, reading); return;
        case GlucoseFace::NoData: faceNoData(canvas, reading); return;
    }
}

bool glucoseChanged(std::uint64_t previousMillis, std::uint64_t nowMillis) noexcept {
    return (nowMillis / 60000u) != (previousMillis / 60000u);
}

}  // namespace apps
}  // namespace stipple
