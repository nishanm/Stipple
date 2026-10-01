// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/config/GlucoseSourceSettings.h"

#include "stipple/apps/GlucoseCloud.h"

namespace stipple {
namespace config {
namespace {

using apps::glucose::jsonQuoted;
using apps::glucose::SourceKind;

/// Longest login name or password accepted. Generous, and still bounded.
constexpr std::size_t kMaxCredentialBytes = 128;

void member(std::string& out, const char* name, std::string_view value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += jsonQuoted(value);
}

void flag(std::string& out, const char* name, bool value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += value ? "true" : "false";
}

std::string upper(std::string text) {
    for (char& c : text) {
        c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    return text;
}

/// A string field, length-checked, onto `target`. Absent is fine.
bool takeString(const json::Value& glucose, const char* key, std::string& target,
                std::string& error) {
    const json::Value value = glucose[key];
    if (!value.valid()) {
        return true;
    }
    if (!value.isString()) {
        error = std::string("'glucose.") + key + "' must be a string";
        return false;
    }
    std::string text = value.toString();
    if (text.size() > kMaxCredentialBytes) {
        error = std::string("'glucose.") + key + "' is too long";
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20) {
            error = std::string("'glucose.") + key + "' has a control character";
            return false;
        }
    }
    target = std::move(text);
    return true;
}

}  // namespace

std::string glucoseSourceStoredMembers(const GlucoseSettings& s) {
    std::string out;
    out.reserve(256);
    out += "\"source\":";
    out += jsonQuoted(s.source);
    member(out, "dexcomUsername", s.dexcomUsername);
    member(out, "dexcomPassword", s.dexcomPassword);
    member(out, "dexcomServer", s.dexcomServer);
    member(out, "libreEmail", s.libreEmail);
    member(out, "librePassword", s.librePassword);
    member(out, "libreRegion", s.libreRegion);
    member(out, "librePatientId", s.librePatientId);
    member(out, "medtrumEmail", s.medtrumEmail);
    member(out, "medtrumPassword", s.medtrumPassword);
    return out;
}

std::string glucoseSourcePublicMembers(const GlucoseSettings& s) {
    std::string out;
    out.reserve(256);
    out += "\"source\":";
    out += jsonQuoted(s.source);
    member(out, "dexcomUsername", s.dexcomUsername);
    flag(out, "dexcomPasswordSet", !s.dexcomPassword.empty());
    member(out, "dexcomServer", s.dexcomServer);
    member(out, "libreEmail", s.libreEmail);
    flag(out, "librePasswordSet", !s.librePassword.empty());
    member(out, "libreRegion", s.libreRegion);
    member(out, "librePatientId", s.librePatientId);
    member(out, "medtrumEmail", s.medtrumEmail);
    flag(out, "medtrumPasswordSet", !s.medtrumPassword.empty());
    return out;
}

void loadGlucoseSourceSettings(const json::Value& g, GlucoseSettings& out) {
    SourceKind kind = SourceKind::Nightscout;
    out.source = apps::glucose::sourceKindFromName(g["source"].toString(out.source), kind)
                     ? apps::glucose::sourceKindName(kind)
                     : "nightscout";
    out.dexcomUsername = g["dexcomUsername"].toString(out.dexcomUsername);
    out.dexcomPassword = g["dexcomPassword"].toString(out.dexcomPassword);
    out.dexcomServer = g["dexcomServer"].toString(out.dexcomServer);
    if (apps::glucose::dexcomBaseUrl(out.dexcomServer) == nullptr) {
        out.dexcomServer = "us";
    }
    out.libreEmail = g["libreEmail"].toString(out.libreEmail);
    out.librePassword = g["librePassword"].toString(out.librePassword);
    out.libreRegion = upper(g["libreRegion"].toString(out.libreRegion));
    if (apps::glucose::libreHost(out.libreRegion) == nullptr) {
        out.libreRegion = "US";
    }
    out.librePatientId = g["librePatientId"].toString(out.librePatientId);
    out.medtrumEmail = g["medtrumEmail"].toString(out.medtrumEmail);
    out.medtrumPassword = g["medtrumPassword"].toString(out.medtrumPassword);
}

bool applyGlucoseSourceSettings(const json::Value& g, GlucoseSettings& out, std::string& error) {
    if (const json::Value value = g["source"]; value.valid()) {
        SourceKind kind = SourceKind::Nightscout;
        if (!value.isString() || !apps::glucose::sourceKindFromName(value.raw(), kind)) {
            error = "'glucose.source' must be nightscout, dexcom, librelinkup or medtrum";
            return false;
        }
        out.source = apps::glucose::sourceKindName(kind);
    }
    if (!takeString(g, "dexcomUsername", out.dexcomUsername, error) ||
        !takeString(g, "dexcomPassword", out.dexcomPassword, error) ||
        !takeString(g, "libreEmail", out.libreEmail, error) ||
        !takeString(g, "librePassword", out.librePassword, error) ||
        !takeString(g, "librePatientId", out.librePatientId, error) ||
        !takeString(g, "medtrumEmail", out.medtrumEmail, error) ||
        !takeString(g, "medtrumPassword", out.medtrumPassword, error)) {
        return false;
    }
    if (const json::Value value = g["dexcomServer"]; value.valid()) {
        if (!value.isString() || apps::glucose::dexcomBaseUrl(value.raw()) == nullptr) {
            error = "'glucose.dexcomServer' must be us, ous or jp";
            return false;
        }
        out.dexcomServer = value.toString();
    }
    if (const json::Value value = g["libreRegion"]; value.valid()) {
        if (!value.isString() || apps::glucose::libreHost(value.raw()) == nullptr) {
            error = "'glucose.libreRegion' is not a LibreLinkUp region";
            return false;
        }
        out.libreRegion = upper(value.toString());
    }
    return true;
}

apps::glucose::SourceSettings sourceSettingsOf(const GlucoseSettings& s) {
    apps::glucose::SourceSettings out;
    apps::glucose::sourceKindFromName(s.source, out.kind);
    switch (out.kind) {
        case SourceKind::Nightscout:
            out.url = s.url;
            out.apiSecretSha1 = s.apiSecretSha1;
            break;
        case SourceKind::Dexcom:
            out.username = s.dexcomUsername;
            out.password = s.dexcomPassword;
            out.region = s.dexcomServer;
            break;
        case SourceKind::LibreLinkUp:
            out.username = s.libreEmail;
            out.password = s.librePassword;
            out.region = s.libreRegion;
            out.patientId = s.librePatientId;
            break;
        case SourceKind::Medtrum:
            out.username = s.medtrumEmail;
            out.password = s.medtrumPassword;
            break;
    }
    return out;
}

}  // namespace config
}  // namespace stipple
