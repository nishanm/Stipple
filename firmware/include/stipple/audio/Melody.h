// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "stipple/audio/Tone.h"

namespace stipple {
namespace audio {

/// One note of a melody. A frequency of zero is a rest.
struct Note {
    std::uint16_t frequencyHz = 0;
    std::uint16_t durationMillis = 0;
};

/// A short tune, held by value in a fixed array.
///
/// Fixed rather than a vector because a melody is played from the audio tick,
/// and nothing on that path allocates (§38). Sixty-four notes and ten seconds
/// are far past anything an alarm needs - the longest preset on the owner's
/// nightscout-clocks is eleven notes - and they are what keep a pasted-in tune
/// from turning into a stuck buzzer.
struct Melody {
    static constexpr int kMaxNotes = 64;
    static constexpr int kMaxTotalMillis = 10000;

    Note notes[kMaxNotes] = {};
    int count = 0;

    /// Sum of every note and rest.
    int totalMillis() const noexcept;

    /// True when at least one note is not a rest. An all-rest melody is a
    /// silent alarm, which is refused wherever a melody is accepted.
    bool audible() const noexcept;
};

enum class RtttlError : std::uint8_t {
    None,
    Empty,
    TooLong,
    Sections,       ///< not exactly name:defaults:notes
    Name,           ///< 1-20 of [A-Za-z0-9 _-]
    Defaults,       ///< d=, o= and b= must all be present and in range
    Note,           ///< a note that does not parse
    TooManyNotes,
    TooLongToPlay,  ///< longer than Melody::kMaxTotalMillis
    Silent,         ///< nothing but rests
};

/// Human-readable, for API error messages.
const char* describe(RtttlError error) noexcept;

/// Longest RTTTL text accepted anywhere. Settings store the text, so this is
/// also what bounds the configuration.
inline constexpr std::size_t kMaxRtttlBytes = 256;

/// Parse Nokia RTTTL - `name:d=4,o=5,b=125:4e7,p,4e7` - into `out`.
///
/// The dialect is the one nightscout-clock's settings page accepts, so a melody
/// copied from those clocks plays here unchanged: exactly three sections, a
/// name of 1-20 characters from [A-Za-z0-9 _-], all three of d=, o= and b= in
/// the middle section, and notes of [duration]letter[#][.][octave][.].
/// Durations 1, 2, 4, 8, 16, 32; octaves 3-8; 25-900 beats per minute.
///
/// Run when a setting is written, never on the audio path. Leaves `out`
/// empty on any error.
RtttlError parseRtttl(std::string_view text, Melody& out) noexcept;

/// Generates PCM for a melody, a frame at a time, like ToneGenerator.
///
/// The notes are timed by the sample count, not by whoever calls fill(). A
/// melody stepped from the main loop would start each note up to a loop
/// iteration late, which on the 34 ms notes of the "urgent" preset is half a
/// note. Here a note is exactly as long as it says.
///
/// The level is absolute, 0-100, and deliberately independent of the device's
/// volume setting: turning the panel down with − must never silence an alarm.
class MelodyGenerator {
public:
    explicit MelodyGenerator(int sampleRate = 16000) noexcept
        : sampleRate_(sampleRate), tone_(sampleRate) {}

    /// Start from the first note, replacing anything playing.
    void start(const Melody& melody, int levelPercent) noexcept;

    void stop() noexcept;

    bool playing() const noexcept { return index_ < melody_.count; }

    /// Fill up to `count` samples. Returns how many belonged to the melody; the
    /// rest of the buffer is zeroed.
    int fill(std::int16_t* samples, int count) noexcept;

private:
    /// Begin note `index_`, or finish if there is none.
    void beginNote() noexcept;

    int sampleRate_;
    Melody melody_;
    int index_ = 0;
    int restRemaining_ = 0;
    ToneGenerator tone_;
};

}  // namespace audio
}  // namespace stipple
