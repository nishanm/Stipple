// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "stipple/script/IScriptRunner.h"

namespace stipple {

class Canvas;

namespace script {

class ScriptHost;

/// Bounded collection of scripts, each with its own Berry interpreter.
///
/// Each script gets its own interpreter rather than sharing one. Sharing would
/// save a few kilobytes and cost the property that matters: scripts arrive over
/// the network from whoever can reach the device, and one of them clobbering
/// another's globals is a bug nobody would ever find. A measured interpreter is
/// about 4 KB (see ScriptHost's memory test), so isolation is affordable here
/// in a way it would not be on an ESP32.
///
/// A script that fails is kept, not dropped. Its source is what the author
/// needs to fix, and deleting their work because it did not compile would be
/// the worst possible response to a typo.
class ScriptStore final : public IScriptRunner {
public:
    /// Sixteen scripts at roughly 4 KB of interpreter each is about 64 KB,
    /// plus source. Bounded because everything here is bounded, not because
    /// the device is near a limit at this number.
    static constexpr int kMaxScripts = 16;
    static constexpr std::size_t kMaxIdBytes = 48;
    static constexpr std::size_t kMaxNameBytes = 64;

    ScriptStore();
    ~ScriptStore() override;

    ScriptStore(const ScriptStore&) = delete;
    ScriptStore& operator=(const ScriptStore&) = delete;

    /// Insert or replace by id. A replacement keeps its position, so saving an
    /// edit must not shuffle the carousel under the person who made it.
    ScriptPutResult put(std::string id, std::string name, std::string source) override;

    bool remove(std::string_view id) override;
    void clear() override;

    int count() const noexcept override { return static_cast<int>(entries_.size()); }
    int capacity() const noexcept override { return kMaxScripts; }
    bool empty() const noexcept { return entries_.empty(); }

    const Script* at(int index) const noexcept override;
    const Script* find(std::string_view id) const noexcept override;

    /// Draw one frame of a script onto the panel.
    ///
    /// False when it is missing or has failed; the reason is in the script's
    /// `problem`. A failed script is not retried every frame - see ScriptHost -
    /// so this stays false until the source is saved again.
    bool draw(std::string_view id, Canvas& canvas, std::uint64_t elapsedMillis) override;

    bool button(std::string_view id, std::string_view name) override;
    InputMode inputMode(std::string_view id) const override;
    void setEnvironment(const ScriptEnvironment& environment) noexcept override;
    void setAudio(platform::IAudioOutput* audio) noexcept override;
    void setMqtt(IScriptMqtt* mqtt) noexcept override;
    void setHttp(IScriptHttp* http) noexcept override;
    void setMicrophone(platform::IMicrophone* microphone) noexcept override;
    std::uint32_t durationMillis(std::string_view id) override;
    bool has(std::string_view id) const noexcept override;
    std::string_view problem(std::string_view id) const noexcept override;

    std::vector<Setting> settings(std::string_view id) const override;
    std::string settingValue(std::string_view id,
                             std::string_view key) const override;
    bool setSetting(std::string_view id, std::string_view key,
                    std::string_view value) override;

    std::size_t memoryBytes() const noexcept override;
    std::size_t maxSourceBytes() const noexcept override;

    std::uint32_t revision() const noexcept override { return revision_; }
    std::string serialize() const override;
    bool deserialize(std::string_view blob) override;

    /// Collect a script's garbage. Worth doing when it leaves the screen,
    /// which is a moment the device has time to spare and the next frame does
    /// not.
    void collectGarbage(std::string_view id);

private:
    struct Entry {
        Script info;
        std::unique_ptr<ScriptHost> host;

        /// Parsed once, when the source is stored. The web UI asks for these
        /// every time somebody opens a script, and re-reading the header on
        /// each request would be work done over and over for an answer that
        /// only changes when the source does.
        std::vector<Setting> settings;

        /// Parsed alongside them, and for a sharper version of the same
        /// reason: this one is consulted on every button press, which is not
        /// a place to be re-scanning a 16 KB source file.
        InputMode input = InputMode::ActionOnly;
    };

    std::vector<Entry> entries_;

    /// Starts at zero so a host that has never written anything and a store
    /// that has never changed agree, and nothing is written on a boot where
    /// nothing happened.
    std::uint32_t revision_ = 0;

    /// Held rather than pushed straight through, so a script added after the
    /// host's last update still starts with a current clock rather than
    /// 1970 until the next frame.
    ScriptEnvironment environment_;
    platform::IAudioOutput* audio_ = nullptr;
    IScriptMqtt* mqtt_ = nullptr;
    IScriptHttp* http_ = nullptr;
    platform::IMicrophone* microphone_ = nullptr;

    Entry* findEntry(std::string_view id) noexcept;
    void refresh(Entry& entry) noexcept;
};

/// Whether an id is one the device will accept.
///
/// Lowercase letters, digits, dash and underscore. Restrictive because a
/// script id becomes part of an API path and a config key, and the set of
/// characters that are safe in both is smaller than the set that looks
/// harmless.
bool validScriptId(std::string_view id) noexcept;

}  // namespace script
}  // namespace stipple
