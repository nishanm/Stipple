// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/audio/Melody.h"

namespace stipple {
namespace audio {
namespace {

/// Octave 4, C to B, in hundredths of a hertz (A4 = 440 Hz, equal temperament).
///
/// A table rather than pow(): core has no dependencies, and twelve numbers are
/// the whole of the arithmetic. Hundredths so that shifting up four octaves
/// still lands within a hertz.
constexpr std::int32_t kOctave4CentiHz[12] = {
    26163, 27718, 29366, 31113, 32963, 34923, 36999, 39200, 41530, 44000, 46616, 49388,
};

/// Semitone of each letter above C, a..g.
constexpr int kSemitone[7] = {9, 11, 0, 2, 4, 5, 7};

constexpr int kMinOctave = 3;
constexpr int kMaxOctave = 8;
constexpr int kMinBpm = 25;
constexpr int kMaxBpm = 900;
constexpr std::size_t kMaxNameBytes = 20;

bool validDuration(int value) noexcept {
    return value == 1 || value == 2 || value == 4 || value == 8 || value == 16 || value == 32;
}

bool isSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Split off everything before the first `separator`, advancing `rest` past it.
/// Returns false when there is no separator left.
bool splitAt(std::string_view& rest, char separator, std::string_view& head) noexcept {
    const std::size_t at = rest.find(separator);
    if (at == std::string_view::npos) {
        head = rest;
        rest = {};
        return false;
    }
    head = rest.substr(0, at);
    rest.remove_prefix(at + 1);
    return true;
}

/// A whole unsigned number, no sign, no spaces, at most four digits.
bool readNumber(std::string_view text, int& out) noexcept {
    if (text.empty() || text.size() > 4) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (!isDigit(c)) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

std::uint16_t frequencyOf(int semitone, int octave) noexcept {
    std::int32_t centi = kOctave4CentiHz[semitone];
    if (octave >= 4) {
        centi <<= (octave - 4);
    } else {
        centi >>= (4 - octave);
    }
    return static_cast<std::uint16_t>((centi + 50) / 100);
}

}  // namespace

int Melody::totalMillis() const noexcept {
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += notes[i].durationMillis;
    }
    return total;
}

bool Melody::audible() const noexcept {
    for (int i = 0; i < count; ++i) {
        if (notes[i].frequencyHz > 0 && notes[i].durationMillis > 0) {
            return true;
        }
    }
    return false;
}

const char* describe(RtttlError error) noexcept {
    switch (error) {
        case RtttlError::None: return "ok";
        case RtttlError::Empty: return "melody is empty";
        case RtttlError::TooLong: return "melody text is longer than 256 characters";
        case RtttlError::Sections: return "melody must be name:defaults:notes";
        case RtttlError::Name: return "melody name must be 1-20 of letters, digits, space, _ or -";
        case RtttlError::Defaults:
            return "melody defaults need d= (1,2,4,8,16,32), o= (3-8) and b= (25-900)";
        case RtttlError::Note: return "melody has a note that does not parse";
        case RtttlError::TooManyNotes: return "melody has more than 64 notes";
        case RtttlError::TooLongToPlay: return "melody is longer than 10 seconds";
        case RtttlError::Silent: return "melody is only rests";
    }
    return "melody is not valid";
}

RtttlError parseRtttl(std::string_view text, Melody& out) noexcept {
    out.count = 0;

    text = trim(text);
    if (text.empty()) {
        return RtttlError::Empty;
    }
    if (text.size() > kMaxRtttlBytes) {
        return RtttlError::TooLong;
    }

    std::string_view rest = text;
    std::string_view name;
    std::string_view defaults;
    if (!splitAt(rest, ':', name) || !splitAt(rest, ':', defaults) ||
        rest.find(':') != std::string_view::npos) {
        return RtttlError::Sections;
    }
    const std::string_view notes = rest;

    // The name is ignored for playback but checked anyway: the settings page on
    // the owner's other clocks checks it, and a melody that one device accepts
    // and the other refuses is a melody somebody cannot copy across.
    name = trim(name);
    if (name.empty() || name.size() > kMaxNameBytes) {
        return RtttlError::Name;
    }
    for (const char c : name) {
        const char l = lower(c);
        const bool allowed = (l >= 'a' && l <= 'z') || isDigit(c) || c == ' ' || c == '_' ||
                             c == '-';
        if (!allowed) {
            return RtttlError::Name;
        }
    }

    int defaultDuration = 0;
    int defaultOctave = 0;
    int bpm = 0;
    std::string_view settings = defaults;
    for (;;) {
        std::string_view item;
        const bool more = splitAt(settings, ',', item);
        item = trim(item);
        if (item.size() < 3 || item[1] != '=') {
            return RtttlError::Defaults;
        }
        int value = 0;
        if (!readNumber(trim(item.substr(2)), value)) {
            return RtttlError::Defaults;
        }
        switch (lower(item[0])) {
            case 'd': defaultDuration = value; break;
            case 'o': defaultOctave = value; break;
            case 'b': bpm = value; break;
            default: return RtttlError::Defaults;
        }
        if (!more) {
            break;
        }
    }
    if (!validDuration(defaultDuration) || defaultOctave < kMinOctave ||
        defaultOctave > kMaxOctave || bpm < kMinBpm || bpm > kMaxBpm) {
        return RtttlError::Defaults;
    }

    // A whole note is four beats.
    const int wholeMillis = 240000 / bpm;
    int total = 0;

    std::string_view list = notes;
    for (;;) {
        std::string_view token;
        const bool more = splitAt(list, ',', token);
        token = trim(token);
        if (token.empty()) {
            out.count = 0;
            return RtttlError::Note;
        }

        std::size_t at = 0;
        int duration = defaultDuration;
        std::size_t digits = 0;
        while (at + digits < token.size() && isDigit(token[at + digits])) {
            ++digits;
        }
        if (digits > 0) {
            if (!readNumber(token.substr(0, digits), duration) || !validDuration(duration)) {
                out.count = 0;
                return RtttlError::Note;
            }
            at += digits;
        }

        if (at >= token.size()) {
            out.count = 0;
            return RtttlError::Note;
        }
        const char letter = lower(token[at++]);
        const bool isRest = letter == 'p';
        if (!isRest && (letter < 'a' || letter > 'g')) {
            out.count = 0;
            return RtttlError::Note;
        }
        int semitone = isRest ? 0 : kSemitone[letter - 'a'];

        if (at < token.size() && token[at] == '#') {
            if (isRest) {
                out.count = 0;
                return RtttlError::Note;
            }
            ++semitone;
            ++at;
        }

        // The dot is written before the octave in some sources and after it in
        // others; both mean half as long again.
        bool dotted = false;
        if (at < token.size() && token[at] == '.') {
            dotted = true;
            ++at;
        }
        int octave = defaultOctave;
        if (at < token.size() && isDigit(token[at])) {
            octave = token[at] - '0';
            ++at;
            if (octave < kMinOctave || octave > kMaxOctave) {
                out.count = 0;
                return RtttlError::Note;
            }
        }
        if (at < token.size() && token[at] == '.' && !dotted) {
            dotted = true;
            ++at;
        }
        if (at != token.size()) {
            out.count = 0;
            return RtttlError::Note;
        }

        // B# is the C above.
        if (semitone == 12) {
            semitone = 0;
            ++octave;
            if (octave > kMaxOctave) {
                out.count = 0;
                return RtttlError::Note;
            }
        }

        int millis = wholeMillis / duration;
        if (dotted) {
            millis += millis / 2;
        }
        total += millis;
        if (total > Melody::kMaxTotalMillis) {
            out.count = 0;
            return RtttlError::TooLongToPlay;
        }
        if (out.count >= Melody::kMaxNotes) {
            out.count = 0;
            return RtttlError::TooManyNotes;
        }
        Note& note = out.notes[out.count++];
        note.frequencyHz = isRest ? 0 : frequencyOf(semitone, octave);
        note.durationMillis = static_cast<std::uint16_t>(millis);

        if (!more) {
            break;
        }
    }

    if (!out.audible()) {
        out.count = 0;
        return RtttlError::Silent;
    }
    return RtttlError::None;
}

// --- generator ---------------------------------------------------------------

void MelodyGenerator::start(const Melody& melody, int levelPercent) noexcept {
    melody_ = melody;
    tone_.setVolumePercent(levelPercent);
    index_ = 0;
    beginNote();
}

void MelodyGenerator::stop() noexcept {
    tone_.stop();
    restRemaining_ = 0;
    index_ = melody_.count;
}

void MelodyGenerator::beginNote() noexcept {
    tone_.stop();
    restRemaining_ = 0;
    // Zero-length notes are skipped here rather than left to stall fill().
    while (index_ < melody_.count) {
        const Note& note = melody_.notes[index_];
        if (note.durationMillis == 0) {
            ++index_;
            continue;
        }
        if (note.frequencyHz == 0) {
            restRemaining_ = (sampleRate_ / 1000) * note.durationMillis;
        } else {
            tone_.start(note.frequencyHz, note.durationMillis);
        }
        return;
    }
}

int MelodyGenerator::fill(std::int16_t* samples, int count) noexcept {
    if (samples == nullptr || count <= 0) {
        return 0;
    }

    int position = 0;
    while (position < count && playing()) {
        int produced = 0;
        if (restRemaining_ > 0) {
            produced = restRemaining_ < (count - position) ? restRemaining_ : (count - position);
            for (int i = 0; i < produced; ++i) {
                samples[position + i] = 0;
            }
            restRemaining_ -= produced;
        } else if (tone_.playing()) {
            produced = tone_.fill(samples + position, count - position);
        }

        position += produced;
        if (restRemaining_ == 0 && !tone_.playing()) {
            ++index_;
            beginNote();
        }
    }

    for (int i = position; i < count; ++i) {
        samples[i] = 0;
    }
    return position;
}

}  // namespace audio
}  // namespace stipple
