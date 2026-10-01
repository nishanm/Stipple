// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/apps/GlucoseFacePlan.h"

namespace stipple {
namespace apps {
namespace glucose {

int activeFaceCount(FaceMask mask) noexcept {
    int count = 0;
    for (int i = 0; i < kGlucoseSelectableFaceCount; ++i) {
        if ((mask & (1u << static_cast<unsigned>(i))) != 0) {
            ++count;
        }
    }
    return count;
}

GlucoseFace firstActiveFace(FaceMask mask) noexcept {
    for (int i = 0; i < kGlucoseSelectableFaceCount; ++i) {
        if ((mask & (1u << static_cast<unsigned>(i))) != 0) {
            return glucoseFaceAt(i);
        }
    }
    return GlucoseFace::Hero;
}

GlucoseFace stepActiveFace(FaceMask mask, GlucoseFace face, int direction) noexcept {
    if ((mask & kAllFaces) == 0) {
        return GlucoseFace::Hero;
    }
    const int step = direction < 0 ? -1 : 1;
    int index = static_cast<int>(face);
    if (index >= kGlucoseSelectableFaceCount) {
        // NoData is not a position; stepping from it starts the cycle.
        return step > 0 ? firstActiveFace(mask)
                        : stepActiveFace(mask, firstActiveFace(mask), -1);
    }
    for (int tries = 0; tries < kGlucoseSelectableFaceCount; ++tries) {
        index = (index + step + kGlucoseSelectableFaceCount) % kGlucoseSelectableFaceCount;
        if ((mask & (1u << static_cast<unsigned>(index))) != 0) {
            return glucoseFaceAt(index);
        }
    }
    return face;
}

GlucoseFace activeOrFirst(FaceMask mask, GlucoseFace face) noexcept {
    return faceActive(mask, face) ? face : firstActiveFace(mask);
}

bool cycleSecondsAllowed(int seconds) noexcept {
    for (const int choice : kCycleChoices) {
        if (choice == seconds) {
            return true;
        }
    }
    return false;
}

int scheduleRowAt(const ScheduleRow* rows, std::size_t count, int localMinutes) noexcept {
    if (rows == nullptr || count == 0) {
        return -1;
    }
    int found = static_cast<int>(count) - 1;  // overnight: yesterday's last row
    for (std::size_t i = 0; i < count; ++i) {
        if (rows[i].minutes <= localMinutes) {
            found = static_cast<int>(i);
        }
    }
    return found;
}

}  // namespace glucose
}  // namespace apps
}  // namespace stipple
