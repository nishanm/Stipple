// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseApp.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "stipple/apps/GlucoseFont.h"
#include "stipple/apps/GlucoseModel.h"
#include "stipple/graphics/Canvas.h"
#include "support/GlucoseGoldenInputs.h"
#include "support/Golden.h"
#include "support/TestFramework.h"

using stipple::Canvas;
using stipple::Framebuffer;
using stipple::Rgb;
using stipple::apps::GlucoseFace;
using stipple::apps::glucoseChanged;
using stipple::apps::glucoseFaceAt;
using stipple::apps::glucoseFaceFromName;
using stipple::apps::glucoseFaceName;
using stipple::apps::kGlucoseFaceCount;
using stipple::apps::renderGlucose;
using stipple::apps::scale8;
using stipple::apps::glucose::Band;
using stipple::apps::glucose::band;
using stipple::apps::glucose::kTrendCount;
using stipple::apps::glucose::Reading;
using stipple::apps::glucose::Trend;
using stipple::apps::glucose::trendFromName;
using stipple::apps::glucose::trendName;
namespace fontdata = stipple::apps::glucose::fontdata;
namespace golden = stipple::apps::glucose::golden;
namespace text = stipple::apps::glucose;
namespace colors = stipple::colors;

namespace {

// The reference audit's rules, as numbers with names (nightscout-pixbar audit.py).
constexpr std::uint8_t kNightLevel = 60;  ///< the dim level a night-time panel realistically runs at
constexpr std::uint8_t kFloorLevel = 12;  ///< worst case
constexpr int kMinLit = 12;               ///< fewer lit pixels than this is a face saying nothing

Reading readingFor(const golden::State& state) {
    Reading reading;
    reading.sgv = state.sgv;
    reading.trend = trendFromName(state.trend);
    reading.hasDelta = state.hasDelta;
    reading.delta = state.delta;
    reading.minutesAgo = state.minutesAgo;
    reading.hour = state.hour;
    reading.minute = state.minute;
    reading.now = golden::kFixedNow;
    for (int i = 0; i < state.historyCount; ++i) {
        reading.pushSample({state.history[i].epoch, state.history[i].sgv});
    }
    return reading;
}

const golden::State* stateNamed(const char* name) {
    for (const golden::State& state : golden::kStates) {
        if (std::strcmp(state.name, name) == 0) {
            return &state;
        }
    }
    return nullptr;
}

Framebuffer render(const Reading& reading, GlucoseFace face) {
    Framebuffer framebuffer;
    Canvas canvas(framebuffer);
    renderGlucose(canvas, reading, face);
    return framebuffer;
}

int litPixels(const Framebuffer& framebuffer) {
    int lit = 0;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            if (framebuffer.at(x, y) != colors::kBlack) {
                ++lit;
            }
        }
    }
    return lit;
}

Framebuffer dimmed(const Framebuffer& framebuffer, std::uint8_t level) {
    Framebuffer out;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            out.set(x, y, scale8(framebuffer.at(x, y), level));
        }
    }
    return out;
}

bool columnLit(const Framebuffer& framebuffer, int x) {
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        if (framebuffer.at(x, y) != colors::kBlack) {
            return true;
        }
    }
    return false;
}

/// The graphs are full-bleed by design; anything else touching column 0 or 51
/// is one pixel from being clipped.
bool fullBleed(GlucoseFace face) {
    return face == GlucoseFace::HeroGraph || face == GlucoseFace::BigGraph;
}

bool mustSurviveFloor(const char* state) {
    return std::strcmp(state, "urgent-low") == 0 || std::strcmp(state, "urgent-high") == 0 ||
           std::strcmp(state, "stale") == 0;
}

std::vector<std::uint8_t> readFixture(const char* name) {
    const std::string path = std::string(STIPPLE_TESTDATA_DIR) + "/" + name + ".rgb";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

/// A check that names the frame it is about, since one test walks all 54.
void expect(bool ok, const golden::Frame& frame, const char* what, const char* file, int line) {
    if (!ok) {
        stipple::test::Registry::instance().fail(
            file, line, std::string(frame.fixture) + ": " + what);
    }
}

#define EXPECT_FRAME(ok, frame, what) expect((ok), (frame), (what), __FILE__, __LINE__)

std::string deltaText(const Reading& reading) {
    char out[8];
    reading.deltaText(out, sizeof out);
    return out;
}

std::string valueText(const Reading& reading) {
    char out[8];
    reading.valueText(out, sizeof out);
    return out;
}

}  // namespace

// --- model --------------------------------------------------------------------

STIPPLE_TEST(Glucose, BandsMatchTheClocks) {
    // Inclusive at the urgent ends, exclusive at the warning ends - the TC001 rule.
    STIPPLE_CHECK(band(55) == Band::Urgent);
    STIPPLE_CHECK(band(56) == Band::Warning);
    STIPPLE_CHECK(band(69) == Band::Warning);
    STIPPLE_CHECK(band(70) == Band::Normal);
    STIPPLE_CHECK(band(180) == Band::Normal);
    STIPPLE_CHECK(band(181) == Band::Warning);
    STIPPLE_CHECK(band(249) == Band::Warning);
    STIPPLE_CHECK(band(250) == Band::Urgent);
}

STIPPLE_TEST(Glucose, StaleIsAStateNotANumber) {
    Reading reading;
    reading.sgv = 104;
    reading.trend = Trend::Flat;
    reading.hasDelta = true;
    reading.delta = 0;

    reading.minutesAgo = 19;
    STIPPLE_CHECK_FALSE(reading.stale());
    STIPPLE_CHECK_EQ(valueText(reading), std::string("104"));
    STIPPLE_CHECK(reading.trendShown() == Trend::Flat);

    reading.minutesAgo = 20;
    STIPPLE_CHECK(reading.stale());
    STIPPLE_CHECK_EQ(valueText(reading), std::string("---"));
    STIPPLE_CHECK_EQ(deltaText(reading), std::string(""));
    STIPPLE_CHECK(reading.trendShown() == Trend::None);
}

STIPPLE_TEST(Glucose, DeltaIsNeverAFabricatedZero) {
    Reading reading;
    reading.sgv = 118;
    reading.hasDelta = false;
    STIPPLE_CHECK_EQ(deltaText(reading), std::string("?"));

    reading.hasDelta = true;
    reading.delta = 2;
    STIPPLE_CHECK_EQ(deltaText(reading), std::string("+2"));

    reading.delta = -15;
    STIPPLE_CHECK_EQ(deltaText(reading), std::string("-15"));
}

STIPPLE_TEST(Glucose, HistoryIsBounded) {
    Reading reading;
    for (int i = 0; i < stipple::apps::glucose::kMaxHistory; ++i) {
        STIPPLE_CHECK(reading.pushSample({i, 100}));
    }
    STIPPLE_CHECK_EQ(reading.historyCount, stipple::apps::glucose::kMaxHistory);
    STIPPLE_CHECK_FALSE(reading.pushSample({999, 100}));
    STIPPLE_CHECK_EQ(reading.historyCount, stipple::apps::glucose::kMaxHistory);
    STIPPLE_CHECK_EQ(reading.history[0].epoch, std::int64_t{1});
    STIPPLE_CHECK_EQ(reading.history.back().epoch, std::int64_t{999});
}

STIPPLE_TEST(Glucose, NamesRoundTrip) {
    for (int i = 0; i < kTrendCount; ++i) {
        const Trend trend = static_cast<Trend>(i);
        STIPPLE_CHECK(trendFromName(trendName(trend)) == trend);
    }
    STIPPLE_CHECK(trendFromName("SIDEWAYS") == Trend::None);

    for (int i = 0; i < kGlucoseFaceCount; ++i) {
        const GlucoseFace face = glucoseFaceAt(i);
        STIPPLE_CHECK(glucoseFaceFromName(glucoseFaceName(face)) == face);
    }
    STIPPLE_CHECK(glucoseFaceFromName("weather") == GlucoseFace::Hero);
}

STIPPLE_TEST(Glucose, RedrawsOnTheMinute) {
    STIPPLE_CHECK_FALSE(glucoseChanged(60000, 119999));
    STIPPLE_CHECK(glucoseChanged(119999, 120000));
}

// --- fonts --------------------------------------------------------------------

STIPPLE_TEST(Glucose, FontsCarryTheReferenceMetrics) {
    // The hero digit is the reason this app has its own text engine: fifteen
    // rows, where the core font engine allows eight.
    STIPPLE_CHECK_EQ(text::textHeight(fontdata::kHero, "?"), 15);
    STIPPLE_CHECK_EQ(text::textHeight(fontdata::kMid, "?"), 10);
    STIPPLE_CHECK_EQ(text::textHeight(fontdata::kSmall, "?"), 8);
    STIPPLE_CHECK_EQ(text::textHeight(fontdata::kHero, "118"), 15);

    // 48 and 287 are different widths; nothing may assume a fixed one.
    STIPPLE_CHECK(text::textWidth(fontdata::kHero, "48") != text::textWidth(fontdata::kHero, "287"));
    STIPPLE_CHECK_EQ(text::textWidth(fontdata::kHero, ""), 0);
    STIPPLE_CHECK_EQ(text::textWidth(fontdata::kHero, "~"), 0);  // not in the table, skipped

    // Ink trimming plus one pixel of tracking: two ten-wide digits are 21.
    STIPPLE_CHECK_EQ(text::textWidth(fontdata::kHero, "00"), 21);
    // A blank glyph advances three, and the tracking either side of it still applies.
    STIPPLE_CHECK_EQ(text::textWidth(fontdata::kHero, "0 0"), 10 + 1 + 3 + 1 + 10);
}

// --- the golden gate ------------------------------------------------------------

STIPPLE_TEST(Glucose, EveryFrameMatchesTheReferenceCorpus) {
    // Six faces in nine states, byte for byte against what nightscout-pixbar
    // drew for the same inputs. This is the whole reason Track B may call its
    // faces the same faces.
    for (const golden::Frame& frame : golden::kFrames) {
        const golden::State* state = stateNamed(frame.state);
        STIPPLE_REQUIRE(state != nullptr);
        const Framebuffer framebuffer = render(readingFor(*state), glucoseFaceFromName(frame.face));
        STIPPLE_CHECK_GOLDEN(frame.fixture, framebuffer);
        EXPECT_FRAME(litPixels(framebuffer) == frame.litPixels, frame,
                     "lit pixel count differs from the manifest");
    }
}

STIPPLE_TEST(Glucose, GoldenComparisonNoticesOnePixel) {
    // A gate is worth something only if it can fail: the fixture bytes are the
    // frame's bytes, and one planted pixel is enough to part them.
    const std::vector<std::uint8_t> fixture = readFixture("glucose-hero--in-range");
    STIPPLE_REQUIRE(fixture.size() == Framebuffer::kByteSize);

    const golden::State* state = stateNamed("in-range");
    STIPPLE_REQUIRE(state != nullptr);
    Framebuffer framebuffer = render(readingFor(*state), GlucoseFace::Hero);
    STIPPLE_CHECK(std::memcmp(framebuffer.bytes(), fixture.data(), Framebuffer::kByteSize) == 0);

    framebuffer.set(0, 0, colors::kWhite);
    STIPPLE_CHECK(std::memcmp(framebuffer.bytes(), fixture.data(), Framebuffer::kByteSize) != 0);
}

// --- the audit gate -------------------------------------------------------------

STIPPLE_TEST(Glucose, AuditRulesHold) {
    // nightscout-pixbar's audit.py, applied to this renderer's output: the
    // faults that only show on hardware, caught on the host instead.
    for (const golden::Frame& frame : golden::kFrames) {
        const golden::State* state = stateNamed(frame.state);
        STIPPLE_REQUIRE(state != nullptr);
        const GlucoseFace face = glucoseFaceFromName(frame.face);
        const Framebuffer framebuffer = render(readingFor(*state), face);

        if (!fullBleed(face)) {
            EXPECT_FRAME(!columnLit(framebuffer, 0) && !columnLit(framebuffer, Framebuffer::kWidth - 1),
                         frame, "ink in an edge column on a face that is not full-bleed");
        }

        const int lit = litPixels(framebuffer);
        EXPECT_FRAME(lit >= kMinLit, frame, "a face saying almost nothing");

        // At the night level at least half the ink must survive, or the face
        // was drawn in colours the panel cannot show after dark.
        const int night = litPixels(dimmed(framebuffer, kNightLevel));
        EXPECT_FRAME(night * 2 >= lit, frame, "more than half the pixels vanish at the night level");

        // The state itself must survive the floor, whatever the decoration does.
        if (mustSurviveFloor(frame.state)) {
            EXPECT_FRAME(litPixels(dimmed(framebuffer, kFloorLevel)) > 0, frame,
                         "nothing visible at the floor level");
        }
    }
}

STIPPLE_TEST(Glucose, Scale8IsTheReferenceRule) {
    // (c * (factor + 1)) >> 8, not c * factor / 255: the two disagree by one in
    // places, and one is what the goldens and the audit were judged with.
    STIPPLE_CHECK_EQ(scale8(Rgb{255, 255, 255}, 34), (Rgb{34, 34, 34}));
    STIPPLE_CHECK_EQ(scale8(Rgb{255, 255, 255}, 150), (Rgb{150, 150, 150}));
    STIPPLE_CHECK_EQ(scale8(Rgb{164, 161, 164}, 14), (Rgb{9, 9, 9}));
    STIPPLE_CHECK_EQ(scale8(Rgb{255, 0, 0}, 255), (Rgb{255, 0, 0}));
    STIPPLE_CHECK_EQ(scale8(Rgb{255, 0, 0}, 0), (Rgb{0, 0, 0}));
}
