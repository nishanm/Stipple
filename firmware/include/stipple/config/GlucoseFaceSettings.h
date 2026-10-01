// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "stipple/apps/GlucoseFacePlan.h"
#include "stipple/config/Config.h"
#include "stipple/json/Json.h"

namespace stipple {
namespace config {

/// The glucose face settings - which faces are in use, the default, cycling
/// and the daily schedule - in one place, so the stored file, the API's GET and
/// its PATCH cannot disagree about the shape or the rules.
///
/// The rules, all enforced here:
/// - at least one face in use, every name known, NoData never among them;
/// - the default face is one of them;
/// - cycling is 0 or one of the TC001's six intervals, and needs two faces;
/// - at most six schedule rows, times 0-1439 and distinct, faces in use,
///   brightness -1 (leave it) or 0-255; an enabled schedule has a row.

/// `"face":...,"faces":[...],"cycleSeconds":N,"schedule":{...}` - members to
/// splice into a `glucose` object, without the braces.
std::string glucoseFaceMembersJson(const GlucoseSettings& settings);

/// Forgiving, for a stored file: anything that fails today's rules is
/// repaired to the nearest thing that passes, never refused, because a load
/// failure would cost every other setting.
void loadGlucoseFaceSettings(const json::Value& glucose, GlucoseSettings& out);

/// Strict, for PATCH: applies whatever of face / faces / cycleSeconds /
/// schedule is present onto `out`, then checks the whole. On failure `error`
/// names the field and `out` must be discarded by the caller.
///
/// One convenience: a PATCH that narrows `faces` without naming `face` moves
/// the default to the first face left, rather than refusing a request whose
/// meaning is plain.
bool applyGlucoseFaceSettings(const json::Value& glucose, GlucoseSettings& out,
                              std::string& error);

/// The mask the host works with.
apps::glucose::FaceMask faceMaskOf(const GlucoseSettings& settings) noexcept;

/// The schedule as the plan's rows, in order, at most six. Returns the count.
std::size_t scheduleRowsOf(const GlucoseSettings& settings, apps::glucose::ScheduleRow* out,
                           std::size_t capacity) noexcept;

}  // namespace config
}  // namespace stipple
