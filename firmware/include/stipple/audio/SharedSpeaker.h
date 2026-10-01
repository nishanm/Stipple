// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "stipple/platform/PlatformServices.h"

namespace stipple {
namespace audio {

/// The speaker as everything except the glucose alarm sees it.
///
/// There is one speaker, and a tone replaces whatever it was playing. A
/// notification chime, the clock's tick, the volume beep or a script's tone
/// arriving mid-alarm would cut the alarm off - and the tick arrives once a
/// second whenever Back has landed on the clock. So while the alarm holds the
/// speaker, their requests are refused here, before they reach it.
///
/// The platform refuses them too, while a melody is actually playing. This
/// wrapper is the part the simulator can test, and it also covers the
/// fraction of a second either side of a melody that the platform cannot see.
class SharedSpeaker final : public platform::IAudioOutput {
public:
    void attach(platform::IAudioOutput* speaker) noexcept { speaker_ = speaker; }

    /// The alarm owns the speaker until `untilMillis`. Zero releases it.
    void holdUntil(std::uint64_t untilMillis) noexcept { heldUntilMillis_ = untilMillis; }

    /// The host's clock, updated once a tick.
    void setNow(std::uint64_t nowMillis) noexcept { nowMillis_ = nowMillis; }

    bool held() const noexcept { return nowMillis_ < heldUntilMillis_; }

    bool playTone(int frequencyHz, int durationMillis) override {
        if (speaker_ == nullptr || held()) {
            return false;
        }
        return speaker_->playTone(frequencyHz, durationMillis);
    }

    bool playSound(std::string_view name) override {
        if (speaker_ == nullptr || held()) {
            return false;
        }
        return speaker_->playSound(name);
    }

    void stop() override {
        if (speaker_ != nullptr && !held()) {
            speaker_->stop();
        }
    }

    void setVolume(std::uint8_t volume) override {
        if (speaker_ != nullptr) {
            speaker_->setVolume(volume);
        }
    }

    std::uint8_t volume() const override {
        return speaker_ != nullptr ? speaker_->volume() : 0;
    }

    // playMelody and stopMelody keep the refusing defaults: the alarm talks to
    // the platform speaker directly, and nothing holding this one may.

private:
    platform::IAudioOutput* speaker_ = nullptr;
    std::uint64_t heldUntilMillis_ = 0;
    std::uint64_t nowMillis_ = 0;
};

}  // namespace audio
}  // namespace stipple
