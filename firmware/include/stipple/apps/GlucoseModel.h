// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace stipple {
namespace apps {
namespace glucose {

/// What a glucose reading IS, independent of where it came from or how it is
/// drawn. A port of nightscout-pixbar's `model.py`; the thresholds and rules are
/// the ones the owner's TC001 clocks run, so a value means the same thing on
/// every panel in the house.

/// mg/dL. Inclusive at the urgent ends, exclusive at the warning ends - see band().
inline constexpr int kUrgentLow = 55;
inline constexpr int kWarnLow = 70;
inline constexpr int kWarnHigh = 180;
inline constexpr int kUrgentHigh = 250;

/// A reading older than this is a *state*, shown as such, never a frozen number.
inline constexpr int kStaleMinutes = 20;

/// Three hours of five-minute CGM readings is 36 points. Dexcom Share repeats a
/// reading about eleven seconds later, so a live history can carry more than
/// the nominal count; 64 leaves room without inviting an unbounded buffer.
inline constexpr int kMaxHistory = 64;

/// The only three things a value can be. The face decides what colour a band is.
enum class Band : std::uint8_t { Normal, Warning, Urgent };

constexpr Band band(int sgv) noexcept {
    if (sgv <= kUrgentLow || sgv >= kUrgentHigh) {
        return Band::Urgent;
    }
    if (sgv < kWarnLow || sgv > kWarnHigh) {
        return Band::Warning;
    }
    return Band::Normal;
}

/// The trend vocabulary sources map into and faces dispatch on. Same set and
/// order as the TC001 firmware's BG_TREND.
enum class Trend : std::uint8_t {
    DoubleUp,
    SingleUp,
    FortyFiveUp,
    Flat,
    FortyFiveDown,
    SingleDown,
    DoubleDown,
    None,
};

inline constexpr int kTrendCount = 8;

inline const char* trendName(Trend trend) noexcept {
    switch (trend) {
        case Trend::DoubleUp: return "DOUBLE_UP";
        case Trend::SingleUp: return "SINGLE_UP";
        case Trend::FortyFiveUp: return "FORTY_FIVE_UP";
        case Trend::Flat: return "FLAT";
        case Trend::FortyFiveDown: return "FORTY_FIVE_DOWN";
        case Trend::SingleDown: return "SINGLE_DOWN";
        case Trend::DoubleDown: return "DOUBLE_DOWN";
        case Trend::None: break;
    }
    return "NONE";
}

/// Unknown names are `None`: a direction we cannot name is a direction we do
/// not draw, never a guess.
inline Trend trendFromName(std::string_view name) noexcept {
    for (int i = 0; i < kTrendCount; ++i) {
        const Trend candidate = static_cast<Trend>(i);
        if (name == trendName(candidate)) {
            return candidate;
        }
    }
    return Trend::None;
}

/// One reading as a source reports it: when and what.
struct Sample {
    std::int64_t epoch = 0;
    int sgv = 0;
};

/// What a face is asked to draw: the newest value with its context, as of `now`.
///
/// Plain data with a fixed-capacity history, so a Reading can live in the host
/// and be handed to the renderer by const reference with no allocation on the
/// frame path. `minutesAgo` is stored rather than derived so a demo state can
/// say exactly how old it is.
struct Reading {
    int sgv = 0;
    Trend trend = Trend::None;

    /// `hasDelta == false` means "cannot be stated honestly" and renders as
    /// `?`, never as +0. A zero delta on a falling glucose is worse than none.
    bool hasDelta = false;
    int delta = 0;

    int minutesAgo = 0;

    /// Wall-clock time for the clock face. `timeKnown == false` draws `--:--`,
    /// the same honesty the clock app shows before SNTP.
    bool timeKnown = true;
    int hour = 0;
    int minute = 0;

    /// The moment the reading is judged at. Graphs plot age against this.
    std::int64_t now = 0;

    std::array<Sample, kMaxHistory> history{};
    int historyCount = 0;

    bool stale() const noexcept { return minutesAgo >= kStaleMinutes; }

    /// A stale reading has no direction worth drawing.
    Trend trendShown() const noexcept { return stale() ? Trend::None : trend; }

    /// Append, dropping the oldest when full. Returns false if a point was dropped.
    bool pushSample(Sample sample) noexcept {
        if (historyCount < kMaxHistory) {
            history[static_cast<std::size_t>(historyCount)] = sample;
            ++historyCount;
            return true;
        }
        for (std::size_t i = 1; i < history.size(); ++i) {
            history[i - 1] = history[i];
        }
        history[history.size() - 1] = sample;
        return false;
    }

    /// "---" when stale, else the value. `out` needs at least 8 bytes.
    int valueText(char* out, std::size_t size) const noexcept {
        if (stale()) {
            return std::snprintf(out, size, "---");
        }
        return std::snprintf(out, size, "%d", sgv);
    }

    /// "" when stale, "?" when unknown, else signed ("+2", "-15"). `out` needs
    /// at least 8 bytes.
    int deltaText(char* out, std::size_t size) const noexcept {
        if (stale()) {
            return std::snprintf(out, size, "%s", "");
        }
        if (!hasDelta) {
            return std::snprintf(out, size, "?");
        }
        return std::snprintf(out, size, "%+d", delta);
    }
};

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
