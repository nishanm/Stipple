// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "stipple/audio/Tone.h"

namespace stipple {
namespace audio {

/// One step of a sound: a note, or a rest.
///
/// A rest is `frequencyHz == 0`, and it matters more than it looks. Two notes
/// at the same pitch with nothing between them are one long note; the gap is
/// what makes a double beep a double beep.
struct ToneStep {
    /// 0 for a rest.
    std::int16_t frequencyHz = 0;
    std::int16_t durationMillis = 0;

    /// Per-note scale, 0-1000, applied on top of the volume setting. 0 means
    /// "unset" and is treated as full, so the common case writes nothing.
    std::int16_t gainPermille = 0;
};

/// A named sound, as a bounded sequence of steps.
///
/// Core rather than platform, and that is the whole point of this file. What
/// "chime" sounds like is not a fact about SigmaStar hardware any more than
/// what a beep sounds like is - `ToneGenerator` already says so - yet the
/// catalogue used to live as an if-chain inside `Tc002Audio`, with a second
/// and different one in the simulator. Two lists of the same thing is two
/// answers to "what does alert sound like", and the simulator was the one
/// people developed against.
///
/// Fixed capacity, no allocation, so a sound can be resolved and started from
/// the render loop (blueprint §38).
struct Sound {
    /// Eight is enough for every sound worth putting on a clock and small
    /// enough that the whole catalogue is a few hundred bytes. A sound that
    /// wants to be a tune is a script's job, not a built-in's.
    static constexpr std::size_t kMaxSteps = 8;

    std::string_view name;
    ToneStep steps[kMaxSteps] = {};
    std::size_t stepCount = 0;

    /// Total length, for callers that want to know how long they are
    /// committing the speaker for.
    int durationMillis() const noexcept;
};

/// The built-in sounds, in one place.
///
/// Deliberately a lookup rather than an enum: the name arrives as a string
/// from configuration, from `/api/v1/sound`, from a notification's `sound`
/// field and from a script, and every one of those paths wants the same
/// answer to "is this a sound this device can make".
class SoundLibrary {
public:
    /// Null when the name is not one of ours.
    ///
    /// Callers must not substitute a default. Naming a sound we cannot make
    /// and getting a beep is the same confident lie as a battery reading 0%
    /// because nothing answered (ADR 0013) - the caller believes it asked for
    /// something specific.
    static const Sound* find(std::string_view name) noexcept;

    /// The whole catalogue, for a UI that wants to offer it and for the test
    /// that asserts the adapters cannot disagree about it.
    static const Sound* all(std::size_t& countOut) noexcept;

    /// Whether `name` is `find()`-able, or the explicit silence "none".
    ///
    /// "none" is a real choice and not the absence of one: a clock in a
    /// bedroom should be able to say nothing, which is why the setting is a
    /// string rather than a bool.
    static bool isValidSetting(std::string_view name) noexcept;
};

/// Plays a `Sound` by driving a `ToneGenerator` one step at a time.
///
/// The generator produces a single tone and knows nothing about sequences, and
/// that division is deliberate: the arithmetic that makes a sine wave has no
/// business also tracking which note of an arpeggio is due. This owns the
/// sequence and hands the generator one note at a time.
///
/// Same pull model as everything else on the audio path - `fill()` is called
/// once per frame and returns immediately, because blueprint §16 does not let
/// audio block rendering and a one-second sound is a hundred and twenty
/// frames.
class MelodyPlayer {
public:
    explicit MelodyPlayer(int sampleRate = 16000) noexcept
        : tone_(sampleRate), sampleRate_(sampleRate) {}

    /// Start a sound. Replaces whatever was playing, for the reason
    /// `ToneGenerator::start` does: a device that queued alerts would fall
    /// behind the thing it is alerting about.
    void start(const Sound& sound) noexcept;

    /// Start a single note, bypassing the catalogue. What `tone()` in a script
    /// and an inline tone on the API reach.
    void startTone(int frequencyHz, int durationMillis, int gainPermille = 1000) noexcept;

    void stop() noexcept;

    bool playing() const noexcept;

    void setVolumePercent(int percent) noexcept { tone_.setVolumePercent(percent); }
    int volumePercent() const noexcept { return tone_.volumePercent(); }

    /// Fill up to `count` samples, advancing through the sequence as steps
    /// finish. Returns how many were written; a short return means the whole
    /// sound ended inside this frame.
    ///
    /// A rest writes silence rather than returning short, so the caller keeps
    /// feeding the driver at a steady rate through a gap. Returning short
    /// would let the driver run dry mid-sound, and on a six-frame buffer that
    /// is audible.
    int fill(std::int16_t* samples, int count) noexcept;

private:
    /// Load `index_` into the generator, or finish. Returns false when the
    /// sequence is done.
    bool beginStep() noexcept;

    ToneGenerator tone_;

    /// Held separately because a rest is counted in samples here rather than
    /// handed to the generator, which has no concept of silence and would
    /// have to grow one for no other reason.
    int sampleRate_;

    /// Copied rather than referenced. The catalogue is static and would be
    /// safe to point at, but an inline tone from the API is not, and one
    /// lifetime rule is better than two.
    Sound current_ = {};
    std::size_t index_ = 0;

    /// Samples of silence still owed for the current rest.
    int restRemaining_ = 0;
};

}  // namespace audio
}  // namespace stipple
