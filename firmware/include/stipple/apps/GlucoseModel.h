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

/// One reading as a source reports it: when, what, which way.
struct Sample {
    std::int64_t epoch = 0;
    int sgv = 0;
    Trend trend = Trend::None;
};

/// Change since the previous reading, or none when it cannot be stated.
///
/// Not "latest minus previous": Dexcom Share returns the same reading twice
/// about eleven seconds apart, so the previous element is often the current
/// one's twin and the delta would come out zero. The firmware takes everything
/// inside a 6.5-minute window and works from its min and max, which is immune
/// to that. `samples` are oldest first.
inline constexpr std::int64_t kDeltaWindowSeconds = 6 * 60 + 30;

inline bool deltaFor(const Sample* samples, int count, int& delta) noexcept {
    if (count < 2) {
        return false;
    }
    const Sample& last = samples[count - 1];
    int first = 0;
    while (first < count && last.epoch - samples[first].epoch > kDeltaWindowSeconds) {
        ++first;
    }
    int inWindow = count - first;
    if (inWindow > 5) {  // Libre-style dense data: just the last two
        first = count - 2;
        inWindow = 2;
    }
    if (inWindow < 2) {
        return false;
    }
    int lo = samples[first].sgv;
    int hi = samples[first].sgv;
    for (int i = first + 1; i < count; ++i) {
        lo = samples[i].sgv < lo ? samples[i].sgv : lo;
        hi = samples[i].sgv > hi ? samples[i].sgv : hi;
    }
    const int base = last.sgv;
    if (lo != base && hi != base) {
        return false;  // moved both ways inside the window; no honest single number
    }
    const int change = (lo == base) ? base - hi : base - lo;
    if (change > 99 || change < -99) {
        return false;
    }
    delta = change;
    return true;
}

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

/// A reading as of `nowUnix`, built from samples oldest first.
///
/// The newest sample's value and direction, the delta by the window rule, the
/// age from the newest sample, and the whole history for the graphs. Building
/// it again from the same samples with a later `now` is how a reading ages
/// when the source stops answering - no field is ever mutated in place. No
/// samples at all is a reading 999 minutes old: stale by every rule, drawn as
/// the explicit no-data face rather than a plausible number.
inline Reading readingFromSamples(const Sample* samples, int count, std::int64_t nowUnix,
                                  int utcOffsetSeconds, bool timeKnown) noexcept {
    Reading reading;
    reading.now = nowUnix;
    reading.timeKnown = timeKnown;
    if (timeKnown) {
        const std::int64_t local = nowUnix + utcOffsetSeconds;
        std::int64_t secondsOfDay = local % 86400;
        if (secondsOfDay < 0) {
            secondsOfDay += 86400;
        }
        reading.hour = static_cast<int>(secondsOfDay / 3600);
        reading.minute = static_cast<int>((secondsOfDay % 3600) / 60);
    }
    if (count <= 0) {
        reading.minutesAgo = 999;
        return reading;
    }
    const int kept = count < kMaxHistory ? count : kMaxHistory;
    const Sample* start = samples + (count - kept);
    for (int i = 0; i < kept; ++i) {
        reading.pushSample(start[i]);
    }
    const Sample& newest = start[kept - 1];
    reading.sgv = newest.sgv;
    reading.trend = newest.trend;
    reading.hasDelta = deltaFor(start, kept, reading.delta);
    const std::int64_t age = nowUnix - newest.epoch;
    reading.minutesAgo = age > 0 ? static_cast<int>(age / 60) : 0;
    return reading;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
