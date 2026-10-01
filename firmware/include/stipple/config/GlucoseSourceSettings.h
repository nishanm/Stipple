// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "stipple/apps/GlucoseSource.h"
#include "stipple/config/Config.h"
#include "stipple/json/Json.h"

namespace stipple {
namespace config {

/// The glucose source block - which service, and each service's login - in
/// one place, so the stored file, GET and PATCH agree on the shape.
///
/// Passwords are write-only: the stored file has them (the device has to log
/// in after a reboot), the API reports only `...PasswordSet`, and diagnostics
/// never see them.

/// Members for the stored file, passwords included, without braces.
std::string glucoseSourceStoredMembers(const GlucoseSettings& settings);

/// Members for GET /settings: names, servers, `...PasswordSet` booleans.
std::string glucoseSourcePublicMembers(const GlucoseSettings& settings);

/// Forgiving, for a stored file: an unknown source becomes Nightscout, an
/// unknown server or region its default.
void loadGlucoseSourceSettings(const json::Value& glucose, GlucoseSettings& out);

/// Strict, for PATCH. Applies whatever of the block is present onto `out`.
/// An empty password string clears it, as every credential here does.
bool applyGlucoseSourceSettings(const json::Value& glucose, GlucoseSettings& out,
                                std::string& error);

/// What the source needs from the settings, for the chosen service.
apps::glucose::SourceSettings sourceSettingsOf(const GlucoseSettings& settings);

}  // namespace config
}  // namespace stipple
