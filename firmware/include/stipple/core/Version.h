// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace stipple {

/// Semantic version of the firmware (blueprint §34). 0.x while in development.
inline constexpr std::string_view kVersion = "0.2.9";

/// Native API contract version, exposed as `/api/v1`. Bumped only for a
/// breaking change; additive fields do not move it.
inline constexpr int kApiVersion = 1;

/// Build identity. Replaced by the real commit hash at release time; the
/// placeholder is deliberately obvious rather than a plausible-looking fake.
inline constexpr std::string_view kBuildCommit = "unknown";

}  // namespace stipple
