// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/audio/Sound.h"

namespace stipple {
namespace audio {
namespace {

/// Shorthand, so the catalogue below reads as music rather than as struct
/// initialisation.
constexpr ToneStep note(int hz, int ms, int gain = 0) {
    return ToneStep{static_cast<std::int16_t>(hz), static_cast<std::int16_t>(ms),
                    static_cast<std::int16_t>(gain)};
}

constexpr ToneStep rest(int ms) { return ToneStep{0, static_cast<std::int16_t>(ms), 0}; }

/// Quiet enough to live with once a second, which is the only reason the
/// per-note gain exists. At the level of a volume confirmation a clock tick is
/// unbearable, and turning the whole device down to fix it would make
/// everything else inaudible.
constexpr int kTickGain = 180;

/// The catalogue.
///
/// The first five are what `Tc002Audio` used to carry as an if-chain, and
/// `beep`, `tick` and `tock` are unchanged note for note: `tick` and `tock`
/// play every second for as long as the clock is on screen and somebody has
/// already decided they sound right, while `chime` and `alert` were single
/// notes and are now the two-and-three-note shapes their names always implied.
///
/// The rest exist because five sounds cannot say five different things. A
/// device that beeps identically for "saved", "failed" and "someone is at the
/// door" is a device you have to look at to understand, which defeats the
/// point of it making a noise at all. So the set is built around *meaning*:
/// rising for good, falling for bad, repeated for urgent.
const Sound kSounds[] = {
    // --- the original five -------------------------------------------------
    Sound{"beep", {note(880, 120)}, 1, },

    // Two rising notes. A perfect fifth, because it is the interval that
    // sounds deliberate rather than accidental on a small speaker.
    Sound{"chime", {note(880, 110), note(1320, 170)}, 2, },

    // Three at one pitch, fast. Repetition is what reads as urgent - a single
    // long note reads as a drone, which is how the old one-note `alert`
    // managed to be less alarming than `chime`.
    Sound{"alert", {note(660, 90), rest(60), note(660, 90), rest(60), note(660, 140)}, 5, },

    Sound{"tick", {note(2200, 10, kTickGain)}, 1, },

    // A shade lower, so a second sounds like a second rather than like a
    // repeated blip. Real clocks do this because the escapement is not
    // symmetric; here it is on purpose.
    Sound{"tock", {note(1800, 10, kTickGain)}, 1, },

    // --- meaning ----------------------------------------------------------
    // Rising major triad. Short, and the shape does the work: nobody has to
    // be told which of these two means it worked.
    Sound{"success", {note(784, 80), note(988, 80), note(1319, 160)}, 3, },

    // Falling, and the second note held. A minor third down is about as close
    // as two notes get to sounding like "no".
    Sound{"failure", {note(660, 130), note(440, 260)}, 2, },

    // Softer and lower than `chime`, for something that wants attention but
    // not immediately.
    Sound{"notify", {note(660, 90, 700), note(880, 130, 700)}, 2, },

    // Long, repeated, and the only sound here built to be annoying. Nothing
    // uses it yet - it is what a countdown or an alarm would reach for, and
    // it is in the catalogue now so that feature is not also a sound-design
    // task. Eight steps is the cap, and this is what the cap is for.
    Sound{"alarm",
          {note(988, 150), rest(80), note(988, 150), rest(80), note(988, 150), rest(80),
           note(988, 150), rest(300)},
          8, },

    // A rising arpeggio, quiet. For a device announcing that it is awake,
    // which should be pleasant rather than commanding.
    Sound{"startup", {note(523, 70, 600), note(659, 70, 600), note(784, 70, 600),
                      note(1047, 140, 600)}, 4, },
};

constexpr std::size_t kSoundCount = sizeof(kSounds) / sizeof(kSounds[0]);

}  // namespace

int Sound::durationMillis() const noexcept {
    int total = 0;
    for (std::size_t i = 0; i < stepCount && i < kMaxSteps; ++i) {
        total += steps[i].durationMillis;
    }
    return total;
}

const Sound* SoundLibrary::find(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kSoundCount; ++i) {
        if (kSounds[i].name == name) {
            return &kSounds[i];
        }
    }
    return nullptr;
}

const Sound* SoundLibrary::all(std::size_t& countOut) noexcept {
    countOut = kSoundCount;
    return kSounds;
}

bool SoundLibrary::isValidSetting(std::string_view name) noexcept {
    return name == "none" || find(name) != nullptr;
}

// --- MelodyPlayer -----------------------------------------------------------

void MelodyPlayer::start(const Sound& sound) noexcept {
    current_ = sound;
    index_ = 0;
    restRemaining_ = 0;
    tone_.stop();
    beginStep();
}

void MelodyPlayer::startTone(int frequencyHz, int durationMillis, int gainPermille) noexcept {
    current_ = Sound{};
    current_.steps[0] = ToneStep{static_cast<std::int16_t>(frequencyHz),
                                 static_cast<std::int16_t>(durationMillis),
                                 static_cast<std::int16_t>(gainPermille)};
    current_.stepCount = 1;
    index_ = 0;
    restRemaining_ = 0;
    tone_.stop();
    beginStep();
}

void MelodyPlayer::stop() noexcept {
    tone_.stop();
    index_ = current_.stepCount;
    restRemaining_ = 0;
}

bool MelodyPlayer::playing() const noexcept {
    return tone_.playing() || restRemaining_ > 0 || index_ < current_.stepCount;
}

bool MelodyPlayer::beginStep() noexcept {
    while (index_ < current_.stepCount && index_ < Sound::kMaxSteps) {
        const ToneStep& step = current_.steps[index_];
        ++index_;

        if (step.durationMillis <= 0) {
            continue;  // a zero-length step is not a sound; skip it
        }

        if (step.frequencyHz <= 0) {
            // A rest. Counted in samples here rather than handed to the
            // generator, which has no concept of silence and would have to
            // grow one for no other reason.
            restRemaining_ = (step.durationMillis * sampleRate_) / 1000;
            if (restRemaining_ <= 0) {
                continue;
            }
            return true;
        }

        const int gain = step.gainPermille > 0 ? step.gainPermille : 1000;
        tone_.start(step.frequencyHz, step.durationMillis, gain);
        return true;
    }
    return false;
}

int MelodyPlayer::fill(std::int16_t* samples, int count) noexcept {
    int written = 0;

    while (written < count) {
        if (restRemaining_ > 0) {
            int silence = restRemaining_;
            if (silence > count - written) {
                silence = count - written;
            }
            for (int i = 0; i < silence; ++i) {
                samples[written + i] = 0;
            }
            written += silence;
            restRemaining_ -= silence;
            continue;
        }

        if (tone_.playing()) {
            const int got = tone_.fill(samples + written, count - written);
            written += got;
            if (got == 0) {
                // The generator says the note is over but has nothing left to
                // give this frame; fall through to the next step rather than
                // spinning.
                if (!beginStep()) {
                    break;
                }
            }
            continue;
        }

        if (!beginStep()) {
            break;
        }
    }

    return written;
}

}  // namespace audio
}  // namespace stipple
