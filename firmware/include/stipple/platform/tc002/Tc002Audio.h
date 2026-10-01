// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "stipple/audio/Melody.h"
#include "stipple/audio/Tone.h"
#include "stipple/platform/PlatformServices.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// The TC002's speaker, through the vendor's own audio library.
///
/// The device has one: the vendor application plays /res/ui/audio/Tip.mp3 and a
/// person standing next to it hears it navigate. libzkgui.so imports
/// MI_AO_SetPubAttr, MI_AO_Enable, MI_AO_EnableChn and MI_AO_SendFrame from
/// /lib/libmi_ao.so, so that is the path, and there is no ALSA on this device
/// to use instead.
///
/// **The configuration was read off the wire, not guessed.** An LD_PRELOAD
/// capture of the vendor application's ioctls on /dev/mi_ao caught SetPubAttr
/// marshalling a 56-byte block whose contents are legible: 16000 Hz, 16-bit,
/// I2S master, mono, six frames of 128 points, one channel. Those are the
/// numbers below. Handing a driver a struct assembled from a hunch is the kind
/// of thing this project refuses to do; watching the working application do it
/// first is the alternative.
///
/// MI_AUDIO_Frame_t was the one struct the capture could not show, because
/// SendFrame passes a pointer and the frame never crosses the ioctl boundary.
/// It was found by writing the length at one candidate offset at a time until
/// the library stopped complaining - see kFrameLengthOffset.
///
/// **Volume is applied in software.** MI_AO_SetVolume exists and refuses every
/// value it was offered, from -80 to 100, so the samples are scaled instead.
/// That works for decoded audio just as well as for tones, which is where this
/// would have ended up anyway.
///
/// Requires a dynamically linked build: it dlopens a vendor library, which a
/// static binary cannot do.
class Tc002Audio final : public IAudioOutput {
public:
    /// What the vendor application configures, byte for byte.
    static constexpr int kSampleRate = 16000;
    static constexpr int kPointsPerFrame = 128;

    /// Where u32Len sits in MI_AUDIO_Frame_t.
    ///
    /// Found by search rather than by assumption: eBitwidth and eSoundmode take
    /// the first two words, apVirAddr turns out to be a sixteen-entry array
    /// rather than the two a stereo device would need, and the timestamp and
    /// sequence follow it. The library rejects a frame whose length it reads as
    /// zero and says so, which made the search a short one.
    static constexpr int kFrameLengthOffset = 84;

    ~Tc002Audio() override;

    /// Loads the vendor library and configures the device. Returns false if
    /// anything refuses, in which case the platform reports no audio at all
    /// rather than accepting sounds it cannot make (ADR 0013).
    bool open();
    bool isOpen() const noexcept { return channelEnabled_; }
    void close() noexcept;

    /// Hands the driver whatever frames it will take without blocking, and
    /// returns. Call once per frame from the application loop.
    ///
    /// The whole reason this class has a tick: blueprint §16 says audio must
    /// never block rendering, and a second of sound is a hundred and twenty
    /// frames. Queueing them all would stall the panel for a second.
    void tick();

    bool playTone(int frequencyHz, int durationMillis) override;
    bool playSound(std::string_view name) override;
    void stop() override;

    void setVolume(std::uint8_t volume) override;
    std::uint8_t volume() const override { return volume_; }

    /// The alarm path. Takes the speaker from any tone in progress, and while
    /// it plays, tones and named sounds are refused and stop() leaves it be.
    bool playMelody(const audio::Melody& melody, int levelPercent) override;
    void stopMelody() override;

private:
    /// Push one frame. Returns false when the driver would have to wait, which
    /// is the signal to stop until the next tick.
    bool sendFrame();

    void* library_ = nullptr;
    bool channelEnabled_ = false;

    int (*setPubAttr_)(int, const void*) = nullptr;
    int (*enable_)(int) = nullptr;
    int (*enableChn_)(int, int) = nullptr;
    int (*sendFrame_)(int, int, const void*, int) = nullptr;
    int (*clearChnBuf_)(int, int) = nullptr;
    int (*disableChn_)(int, int) = nullptr;
    int (*disable_)(int) = nullptr;

    audio::ToneGenerator tone_{kSampleRate};
    audio::MelodyGenerator melody_{kSampleRate};
    std::uint8_t volume_ = 153;  // 60%, matching the config default

    std::int16_t samples_[kPointsPerFrame] = {};

    /// Big enough for the whole of MI_AUDIO_Frame_t, which reaches at least to
    /// offset 84. Oversized and zeroed rather than declared as a struct: the
    /// layout is known where it matters and unknown after that, and zero is the
    /// only honest value for a field nobody has identified.
    unsigned char frame_[128] = {};
};

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
