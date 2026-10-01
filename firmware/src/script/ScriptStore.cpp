// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/script/ScriptStore.h"

#include "stipple/script/ScriptConfig.h"
#include "stipple/script/IScriptHttp.h"
#include "stipple/script/IScriptMqtt.h"

#include "stipple/script/ScriptHost.h"

#include <cstdio>
#include <cstdlib>

namespace stipple {
namespace script {

bool validScriptId(std::string_view id) noexcept {
    if (id.empty() || id.size() > ScriptStore::kMaxIdBytes) {
        return false;
    }
    for (const char c : id) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '-' || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

ScriptStore::ScriptStore() = default;

// Out of line because Entry holds a unique_ptr<ScriptHost> and ScriptHost is
// only forward declared in the header. The destructor has to be where the
// complete type is.
ScriptStore::~ScriptStore() = default;

ScriptStore::Entry* ScriptStore::findEntry(std::string_view id) noexcept {
    for (Entry& entry : entries_) {
        if (entry.info.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

void ScriptStore::refresh(Entry& entry) noexcept {
    if (entry.host == nullptr) {
        entry.info.ok = false;
        return;
    }
    entry.info.ok = entry.host->ready();
    entry.info.lastInstructions = entry.host->lastInstructions();
    entry.info.memoryBytes = entry.host->memoryBytes();
}

ScriptPutResult ScriptStore::put(std::string id, std::string name, std::string source) {
    if (!validScriptId(id)) {
        return ScriptPutResult::InvalidId;
    }
    if (source.size() > ScriptHost::kMaxSourceBytes) {
        return ScriptPutResult::SourceTooLarge;
    }
    if (name.size() > kMaxNameBytes) {
        name.resize(kMaxNameBytes);
    }

    Entry* existing = findEntry(id);
    if (existing == nullptr && count() >= kMaxScripts) {
        return ScriptPutResult::TooManyScripts;
    }

    // A fresh interpreter every time, including on a replacement. Reusing one
    // would leave the previous script's globals and instances in place, so an
    // edit would run against state the new source never created - and it would
    // appear to work right up until the device restarted.
    auto host = std::make_unique<ScriptHost>();
    std::string problem;
    const bool compiled = host->load(source, problem);

    Entry entry;
    entry.info.id = std::move(id);
    entry.info.name = std::move(name);
    entry.info.source = std::move(source);
    entry.info.problem = problem;
    entry.host = std::move(host);
    // A script saved between frames should not see 1970 on its first one.
    entry.host->setEnvironment(environment_);
    entry.host->setAudio(audio_);
    entry.host->setMqtt(mqtt_, entry.info.id);
    entry.host->setHttp(http_, entry.info.id);
    entry.host->setMicrophone(microphone_);
    entry.settings = parseSettings(entry.info.source);
    entry.input = parseInputMode(entry.info.source);
    refresh(entry);

    // A replacement starts with no watches. The new source may well want
    // different topics, and carrying the old ones over would leave the device
    // subscribed on behalf of code that no longer exists - visible only as a
    // filter in the broker's list that matches nothing in the script.
    if (existing != nullptr) {
        if (mqtt_ != nullptr) {
            mqtt_->forget(entry.info.id);
        }
        if (http_ != nullptr) {
            http_->forget(entry.info.id);
        }
    }

    ++revision_;

    if (existing != nullptr) {
        // Position is kept deliberately: saving an edit must not shuffle the
        // carousel under the person who made it.
        *existing = std::move(entry);
        return compiled ? ScriptPutResult::Replaced : ScriptPutResult::DidNotCompile;
    }

    entries_.push_back(std::move(entry));
    return compiled ? ScriptPutResult::Added : ScriptPutResult::DidNotCompile;
}

bool ScriptStore::remove(std::string_view id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].info.id == id) {
            if (mqtt_ != nullptr) {
                mqtt_->forget(id);
            }
            if (http_ != nullptr) {
                http_->forget(id);
            }
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            ++revision_;
            return true;
        }
    }
    return false;
}

void ScriptStore::clear() {
    if (!entries_.empty()) {
        for (const Entry& entry : entries_) {
            if (mqtt_ != nullptr) {
                mqtt_->forget(entry.info.id);
            }
            if (http_ != nullptr) {
                http_->forget(entry.info.id);
            }
        }
        entries_.clear();
        ++revision_;
    }
}

const Script* ScriptStore::at(int index) const noexcept {
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return &entries_[static_cast<std::size_t>(index)].info;
}

const Script* ScriptStore::find(std::string_view id) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.info.id == id) {
            return &entry.info;
        }
    }
    return nullptr;
}

bool ScriptStore::has(std::string_view id) const noexcept {
    return find(id) != nullptr;
}

std::string_view ScriptStore::problem(std::string_view id) const noexcept {
    const Script* script = find(id);
    if (script == nullptr) {
        return "no such script";
    }
    return script->problem;
}

bool ScriptStore::draw(std::string_view id, Canvas& canvas, std::uint64_t elapsedMillis) {
    Entry* entry = findEntry(id);
    if (entry == nullptr || entry->host == nullptr) {
        return false;
    }
    if (!entry->host->ready()) {
        return false;  // already failed; the reason is in info.problem
    }

    std::string problem;
    const bool drew = entry->host->draw(canvas, elapsedMillis, problem);
    if (!drew) {
        // Keep the first failure. A script that dies on frame one and a script
        // that dies on frame ten thousand need the same message, and it is the
        // one from the frame that actually broke.
        entry->info.problem = problem;
    }
    refresh(*entry);
    return drew;
}

bool ScriptStore::button(std::string_view id, std::string_view name) {
    Entry* entry = findEntry(id);
    if (entry == nullptr || entry->host == nullptr || !entry->host->ready()) {
        return false;
    }

    std::string problem;
    const ScriptHost::EventResult result = entry->host->button(name, problem);
    if (result == ScriptHost::EventResult::Failed) {
        entry->info.problem = problem;
    }
    refresh(*entry);
    return result == ScriptHost::EventResult::Handled;
}

InputMode ScriptStore::inputMode(std::string_view id) const {
    // Deliberately not gated on the script being ready. A script that failed
    // to compile is still the app on screen, and the controls should behave
    // the way its header says while somebody is looking at the error - not
    // silently revert to driving the carousel, which is the one thing that
    // would make the failure hard to read.
    for (const Entry& entry : entries_) {
        if (entry.info.id == id) {
            return entry.input;
        }
    }
    return InputMode::ActionOnly;
}

std::uint32_t ScriptStore::durationMillis(std::string_view id) {
    Entry* entry = findEntry(id);
    if (entry == nullptr || entry->host == nullptr || !entry->host->ready()) {
        return 0;
    }
    return entry->host->durationMillis();
}

void ScriptStore::setEnvironment(const ScriptEnvironment& environment) noexcept {
    environment_ = environment;
    for (Entry& entry : entries_) {
        if (entry.host != nullptr) {
            entry.host->setEnvironment(environment);
        }
    }
}

void ScriptStore::setAudio(platform::IAudioOutput* audio) noexcept {
    audio_ = audio;
    for (Entry& entry : entries_) {
        if (entry.host != nullptr) {
            entry.host->setAudio(audio);
        }
    }
}

void ScriptStore::setMqtt(IScriptMqtt* mqtt) noexcept {
    mqtt_ = mqtt;
    for (Entry& entry : entries_) {
        if (entry.host != nullptr) {
            entry.host->setMqtt(mqtt, entry.info.id);
        }
    }
}

void ScriptStore::setHttp(IScriptHttp* http) noexcept {
    http_ = http;
    for (Entry& entry : entries_) {
        if (entry.host != nullptr) {
            entry.host->setHttp(http, entry.info.id);
        }
    }
}

void ScriptStore::setMicrophone(platform::IMicrophone* microphone) noexcept {
    microphone_ = microphone;
    for (Entry& entry : entries_) {
        if (entry.host != nullptr) {
            entry.host->setMicrophone(microphone);
        }
    }
}


// --- declared settings -------------------------------------------------------

std::vector<Setting> ScriptStore::settings(std::string_view id) const {
    for (const Entry& entry : entries_) {
        if (entry.info.id == id) {
            return entry.settings;
        }
    }
    return {};
}

std::string ScriptStore::settingValue(std::string_view id,
                                      std::string_view key) const {
    for (const Entry& entry : entries_) {
        if (entry.info.id != id || entry.host == nullptr) {
            continue;
        }
        for (const auto& held : entry.host->stored()) {
            if (held.first != key) {
                continue;
            }
            // Rendered as text whatever it is stored as, because that is
            // what a form field round-trips. The declaration says how to
            // read it back.
            switch (held.second.kind) {
                case ScriptHost::Stored::Kind::Text:
                    return held.second.text;
                case ScriptHost::Stored::Kind::Integer:
                    return std::to_string(held.second.integer);
                case ScriptHost::Stored::Kind::Boolean:
                    return held.second.boolean ? "true" : "false";
                case ScriptHost::Stored::Kind::Real: {
                    std::string out = std::to_string(held.second.real);
                    // to_string pads a float to six decimals, and "1.000000"
                    // in a form field is a number somebody has to edit twice.
                    while (out.size() > 1 && out.back() == '0') {
                        out.pop_back();
                    }
                    if (!out.empty() && out.back() == '.') {
                        out.pop_back();
                    }
                    return out;
                }
            }
        }
        return {};
    }
    return {};
}

bool ScriptStore::setSetting(std::string_view id, std::string_view key,
                             std::string_view value) {
    Entry* entry = findEntry(id);
    if (entry == nullptr || entry->host == nullptr) {
        return false;
    }

    // Only declared keys. Without this the API would be a way to write
    // arbitrary entries into a script's private store from outside, which is
    // a different feature with different consequences.
    const Setting* declared = nullptr;
    for (const Setting& setting : entry->settings) {
        if (setting.key == key) {
            declared = &setting;
            break;
        }
    }
    if (declared == nullptr) {
        return false;
    }

    ScriptHost::Stored stored;
    switch (declared->type) {
        case Setting::Type::Text: {
            const std::size_t limit =
                declared->maxLength > 0
                    ? static_cast<std::size_t>(declared->maxLength)
                    : ScriptHost::kMaxStoreTextBytes;
            if (value.size() > limit) {
                return false;
            }
            stored.kind = ScriptHost::Stored::Kind::Text;
            stored.text.assign(value);
            break;
        }
        case Setting::Type::Number: {
            if (value.empty() || value.size() > 11) {
                return false;
            }
            long long parsed = 0;
            bool negative = false;
            std::size_t at = 0;
            if (value[0] == '-') {
                negative = true;
                at = 1;
                if (value.size() == 1) {
                    return false;
                }
            }
            for (; at < value.size(); ++at) {
                if (value[at] < '0' || value[at] > '9') {
                    return false;
                }
                parsed = parsed * 10 + (value[at] - '0');
            }
            if (negative) {
                parsed = -parsed;
            }
            if (declared->bounded &&
                (parsed < declared->minimum || parsed > declared->maximum)) {
                return false;
            }
            stored.kind = ScriptHost::Stored::Kind::Integer;
            stored.integer = static_cast<std::int32_t>(parsed);
            break;
        }
        case Setting::Type::Boolean: {
            // "on" and "1" as well as "true", because that is what a form
            // checkbox and a shell script respectively send.
            const bool yes = value == "true" || value == "1" || value == "on";
            const bool no = value == "false" || value == "0" ||
                            value == "off" || value.empty();
            if (!yes && !no) {
                return false;
            }
            stored.kind = ScriptHost::Stored::Kind::Boolean;
            stored.boolean = yes;
            break;
        }
    }

    if (!entry->host->setStored(key, std::move(stored))) {
        return false;
    }

    // The library changed, so it needs writing back. Without this a setting
    // would survive until the next reboot and then quietly revert, which is
    // the worst of both - it appears to work.
    ++revision_;
    return true;
}

void ScriptStore::collectGarbage(std::string_view id) {
    if (Entry* entry = findEntry(id); entry != nullptr && entry->host != nullptr) {
        entry->info.memoryBytes = entry->host->collectGarbage();
    }
}

std::size_t ScriptStore::maxSourceBytes() const noexcept {
    return ScriptHost::kMaxSourceBytes;
}

std::size_t ScriptStore::memoryBytes() const noexcept {
    std::size_t total = 0;
    for (const Entry& entry : entries_) {
        total += entry.info.memoryBytes;
    }
    return total;
}


// --- persistence -------------------------------------------------------------
//
// Length-prefixed, not delimited. A script is arbitrary text that will contain
// newlines, quotes and very likely whatever separator seemed safe at the time,
// so nothing here scans for one: every field says how long it is and the
// reader takes exactly that many bytes. The format cannot be confused by its
// own contents.

namespace {

/// Stands in for a missing interpreter's values, so serialize() has something
/// to take a reference to rather than a branch around every use.
const std::vector<std::pair<std::string, ScriptHost::Stored>> kNoStoredValues;

constexpr char kMagic[] = "SBS";       // Stipple Berry Scripts
constexpr std::uint8_t kFormatVersion = 2;  // 2 adds each script's stored values

void pushByte(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void pushUint32(std::string& out, std::uint32_t value) {
    pushByte(out, static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    pushByte(out, static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    pushByte(out, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    pushByte(out, static_cast<std::uint8_t>(value & 0xFFu));
}

/// Reads forward through a blob, refusing to run off the end.
///
/// Every read is checked, because this blob comes off storage that may have
/// been interrupted mid-write, and a truncated one must be rejected rather
/// than read past.
class Reader {
public:
    explicit Reader(std::string_view blob) : blob_(blob) {}

    bool byte(std::uint8_t& out) {
        if (at_ >= blob_.size()) { return false; }
        out = static_cast<std::uint8_t>(blob_[at_++]);
        return true;
    }

    bool uint32(std::uint32_t& out) {
        std::uint8_t b[4];
        for (std::uint8_t& each : b) {
            if (!byte(each)) { return false; }
        }
        out = (static_cast<std::uint32_t>(b[0]) << 24) |
              (static_cast<std::uint32_t>(b[1]) << 16) |
              (static_cast<std::uint32_t>(b[2]) << 8) |
              static_cast<std::uint32_t>(b[3]);
        return true;
    }

    bool text(std::uint32_t length, std::string& out) {
        if (length > blob_.size() - at_) { return false; }
        out.assign(blob_, at_, length);
        at_ += length;
        return true;
    }

    bool exhausted() const { return at_ == blob_.size(); }

private:
    std::string_view blob_;
    std::size_t at_ = 0;
};

}  // namespace

std::string ScriptStore::serialize() const {
    std::string out;
    out += kMagic;
    pushByte(out, kFormatVersion);
    pushByte(out, static_cast<std::uint8_t>(entries_.size()));

    for (const Entry& entry : entries_) {
        pushByte(out, static_cast<std::uint8_t>(entry.info.id.size()));
        out += entry.info.id;
        pushByte(out, static_cast<std::uint8_t>(entry.info.name.size()));
        out += entry.info.name;
        pushUint32(out, static_cast<std::uint32_t>(entry.info.source.size()));
        out += entry.info.source;

        // What the script asked the device to remember. A high score that did
        // not survive a power cut is not a high score.
        const auto& stored =
            entry.host != nullptr ? entry.host->stored() : kNoStoredValues;
        pushByte(out, static_cast<std::uint8_t>(stored.size()));
        for (const auto& pair : stored) {
            pushByte(out, static_cast<std::uint8_t>(pair.first.size()));
            out += pair.first;
            pushByte(out, static_cast<std::uint8_t>(pair.second.kind));
            switch (pair.second.kind) {
                case ScriptHost::Stored::Kind::Integer:
                    pushUint32(out, static_cast<std::uint32_t>(pair.second.integer));
                    break;
                case ScriptHost::Stored::Kind::Real: {
                    // Written as the text of the number rather than as raw
                    // float bytes. This blob is read back by the same build
                    // that wrote it today and by a different one after an
                    // update, and a float's byte order is not something to
                    // assume across that.
                    char buffer[32];
                    std::snprintf(buffer, sizeof(buffer), "%.7g",
                                  static_cast<double>(pair.second.real));
                    const std::string text(buffer);
                    pushByte(out, static_cast<std::uint8_t>(text.size()));
                    out += text;
                    break;
                }
                case ScriptHost::Stored::Kind::Boolean:
                    pushByte(out, pair.second.boolean ? 1u : 0u);
                    break;
                case ScriptHost::Stored::Kind::Text:
                    pushByte(out, static_cast<std::uint8_t>(pair.second.text.size()));
                    out += pair.second.text;
                    break;
            }
        }
    }
    return out;
}

bool ScriptStore::deserialize(std::string_view blob) {
    Reader reader(blob);

    for (const char expected : std::string_view(kMagic)) {
        std::uint8_t actual = 0;
        if (!reader.byte(actual) || actual != static_cast<std::uint8_t>(expected)) {
            return false;
        }
    }

    std::uint8_t version = 0;
    if (!reader.byte(version) || version != kFormatVersion) {
        return false;
    }

    std::uint8_t stored = 0;
    if (!reader.byte(stored) || stored > kMaxScripts) {
        return false;
    }

    // Read the whole blob before touching the store. A half-applied restore
    // would leave the device with some of the old library and some of the new,
    // which is worse than either.
    struct Pending {
        std::string id;
        std::string name;
        std::string source;
        std::vector<std::pair<std::string, ScriptHost::Stored>> stored;
    };
    std::vector<Pending> pending;
    pending.reserve(stored);

    for (std::uint8_t i = 0; i < stored; ++i) {
        Pending entry;
        std::uint8_t idLength = 0;
        std::uint8_t nameLength = 0;
        std::uint32_t sourceLength = 0;
        if (!reader.byte(idLength) || !reader.text(idLength, entry.id)) { return false; }
        if (!reader.byte(nameLength) || !reader.text(nameLength, entry.name)) { return false; }
        if (!reader.uint32(sourceLength)) { return false; }
        if (sourceLength > ScriptHost::kMaxSourceBytes) { return false; }
        if (!reader.text(sourceLength, entry.source)) { return false; }
        if (!validScriptId(entry.id)) { return false; }

        std::uint8_t storedCount = 0;
        if (!reader.byte(storedCount) || storedCount > ScriptHost::kMaxStoreKeys) {
            return false;
        }
        for (std::uint8_t k = 0; k < storedCount; ++k) {
            std::string key;
            std::uint8_t keyLength = 0;
            std::uint8_t kind = 0;
            if (!reader.byte(keyLength) || !reader.text(keyLength, key)) { return false; }
            if (!reader.byte(kind) ||
                kind > static_cast<std::uint8_t>(ScriptHost::Stored::Kind::Text)) {
                return false;
            }

            ScriptHost::Stored value;
            value.kind = static_cast<ScriptHost::Stored::Kind>(kind);
            switch (value.kind) {
                case ScriptHost::Stored::Kind::Integer: {
                    std::uint32_t raw = 0;
                    if (!reader.uint32(raw)) { return false; }
                    value.integer = static_cast<std::int32_t>(raw);
                    break;
                }
                case ScriptHost::Stored::Kind::Real: {
                    std::string text;
                    std::uint8_t length = 0;
                    if (!reader.byte(length) || !reader.text(length, text)) { return false; }
                    value.real = std::strtof(text.c_str(), nullptr);
                    break;
                }
                case ScriptHost::Stored::Kind::Boolean: {
                    std::uint8_t raw = 0;
                    if (!reader.byte(raw)) { return false; }
                    value.boolean = raw != 0;
                    break;
                }
                case ScriptHost::Stored::Kind::Text: {
                    std::uint8_t length = 0;
                    if (!reader.byte(length) || !reader.text(length, value.text)) {
                        return false;
                    }
                    break;
                }
            }
            entry.stored.emplace_back(std::move(key), std::move(value));
        }

        pending.push_back(std::move(entry));
    }

    // Trailing bytes mean this is not the blob it claims to be.
    if (!reader.exhausted()) {
        return false;
    }

    entries_.clear();
    for (Pending& entry : pending) {
        // Through put(), so every script is compiled on the way in and a
        // stored script that no longer compiles - because the firmware's
        // builtins changed under it, say - comes back with its source intact
        // and its reason attached, exactly as if it had just been typed.
        const std::string id = entry.id;
        put(std::move(entry.id), std::move(entry.name), std::move(entry.source));

        // After put(), because put() builds a fresh interpreter and would
        // throw the values away if they were restored first.
        if (Entry* restored = findEntry(id);
            restored != nullptr && restored->host != nullptr) {
            restored->host->restoreStored(std::move(entry.stored));
        }
    }
    return true;
}

}  // namespace script
}  // namespace stipple
