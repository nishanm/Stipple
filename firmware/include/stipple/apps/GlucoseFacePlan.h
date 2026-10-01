// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "stipple/apps/GlucoseApp.h"

namespace stipple {
namespace apps {
namespace glucose {

/// Which faces are in use, what the knob moves between, and what the panel
/// shows on its own: the "Clock faces" and "Daily schedule" cards of the TC001
/// nightscout-clock settings page, carried over so the two clocks in the house
/// are configured the same way.
///
/// Pure functions over plain data, so every rule here is tested without a
/// host. The host owns the clock and the timers; this decides.

/// One bit per selectable face, in `GlucoseFace` order. NoData is never in
/// the mask: it is what a stale reading is drawn as, not a choice.
using FaceMask = std::uint8_t;

inline constexpr FaceMask kAllFaces = static_cast<FaceMask>((1u << kGlucoseSelectableFaceCount) - 1u);

constexpr FaceMask faceBit(GlucoseFace face) noexcept {
    return static_cast<int>(face) < kGlucoseSelectableFaceCount
               ? static_cast<FaceMask>(1u << static_cast<unsigned>(face))
               : FaceMask{0};
}

constexpr bool faceActive(FaceMask mask, GlucoseFace face) noexcept {
    return (mask & faceBit(face)) != 0;
}

/// Number of faces in the mask.
int activeFaceCount(FaceMask mask) noexcept;

/// The first face in the mask, in canonical order; Hero for an empty mask
/// (which the settings never store, but a caller must still get a face).
GlucoseFace firstActiveFace(FaceMask mask) noexcept;

/// The next (+1) or previous (-1) face *in the mask*, wrapping. A face that is
/// not in the mask steps to the nearest one in that direction, so a knob turned
/// after the set changed lands somewhere sensible rather than nowhere.
GlucoseFace stepActiveFace(FaceMask mask, GlucoseFace face, int direction) noexcept;

/// `face` if it is in the mask, else the first face that is.
GlucoseFace activeOrFirst(FaceMask mask, GlucoseFace face) noexcept;

/// The cycling intervals the settings accept, in seconds. 0 is "off". The same
/// six the TC001 offers: under ten seconds a face cannot be read before it
/// goes, and over five minutes nobody notices it is cycling.
inline constexpr int kCycleChoices[] = {0, 10, 30, 60, 120, 180, 300};

bool cycleSecondsAllowed(int seconds) noexcept;

/// One row of the daily schedule: from `minutes` after local midnight the
/// panel shows `face` at `brightness` until the next row. The last row runs
/// overnight into the first.
struct ScheduleRow {
    int minutes = 0;
    GlucoseFace face = GlucoseFace::Hero;
    /// 0-255, or -1 to leave the brightness alone during this row.
    int brightness = -1;
};

inline constexpr std::size_t kMaxScheduleRows = 6;

/// Which row applies at `localMinutes` (0-1439). Rows must be sorted by
/// `minutes`. Before the first row of the day, the last row - yesterday
/// evening's - is still in force. -1 when there are no rows.
int scheduleRowAt(const ScheduleRow* rows, std::size_t count, int localMinutes) noexcept;

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
