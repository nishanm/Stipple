// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <string>
#include <vector>

#include "stipple/audio/Melody.h"
#include "stipple/audio/SharedSpeaker.h"
#include "stipple/config/GlucoseAlarmSettings.h"
#include "stipple/platform/simulator/SimulatorPlatform.h"
#include "support/TestFramework.h"

using stipple::audio::Melody;
using stipple::audio::MelodyGenerator;
using stipple::audio::parseRtttl;
using stipple::audio::RtttlError;
using stipple::audio::SharedSpeaker;
using stipple::platform::simulator::SimulatorAudio;

namespace {

int error(RtttlError e) { return static_cast<int>(e); }

RtttlError parse(const std::string& text) {
    Melody melody;
    return parseRtttl(text, melody);
}

}  // namespace

STIPPLE_TEST(Rtttl, EveryDefaultAndPresetFromTheClocksParses) {
    // The defaults and the settings-page presets of nightscout-clock, verbatim.
    // A melody the owner copies from a clock must play here unchanged.
    const char* const melodies[] = {
        stipple::config::kUrgentLowMelody,
        stipple::config::kLowMelody,
        stipple::config::kHighMelody,
        stipple::config::kNoDataMelody,
        "doublebeep:d=8,o=6,b=180:c,p,c",
        "triplebeep:d=16,o=6,b=200:c,p,c,p,c",
        "siren:d=4,o=5,b=100:a,d6,a,d6",
        "urgent:d=32,o=7,b=220:c,p,c,p,c,p,c,p,c,p,c",
        "ping:d=4,o=6,b=140:8e,16p,8c",
        "longtone:d=1,o=5,b=90:a",
    };
    for (const char* text : melodies) {
        Melody melody;
        STIPPLE_CHECK_EQ(error(parseRtttl(text, melody)), error(RtttlError::None));
        STIPPLE_CHECK(melody.count > 0);
        STIPPLE_CHECK(melody.audible());
    }
}

STIPPLE_TEST(Rtttl, NotesHaveTheRightPitchAndLength) {
    Melody melody;
    // b=125: a whole note is 1920 ms, a quarter 480. "4e7" is E7 = 2637 Hz.
    STIPPLE_REQUIRE(parseRtttl("high:d=4,o=5,b=125:4e7,p,4e7", melody) == RtttlError::None);
    STIPPLE_REQUIRE(melody.count == 3);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[0].frequencyHz), 2637);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[0].durationMillis), 480);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[1].frequencyHz), 0);  // a rest
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[1].durationMillis), 480);
    STIPPLE_CHECK_EQ(melody.totalMillis(), 1440);

    // A4, dotted either side of the octave, and a sharp.
    STIPPLE_REQUIRE(parseRtttl("t:d=4,o=4,b=60:a,a.,a4.,8c#5", melody) == RtttlError::None);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[0].frequencyHz), 440);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[0].durationMillis), 1000);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[1].durationMillis), 1500);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[2].durationMillis), 1500);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[3].frequencyHz), 554);
    STIPPLE_CHECK_EQ(static_cast<int>(melody.notes[3].durationMillis), 500);
}

STIPPLE_TEST(Rtttl, MalformedMelodiesAreRefusedWithAReason) {
    STIPPLE_CHECK_EQ(error(parse("")), error(RtttlError::Empty));
    STIPPLE_CHECK_EQ(error(parse("   ")), error(RtttlError::Empty));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100")), error(RtttlError::Sections));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:c:d")), error(RtttlError::Sections));
    STIPPLE_CHECK_EQ(error(parse(":d=4,o=5,b=100:c")), error(RtttlError::Name));
    STIPPLE_CHECK_EQ(error(parse("bad!name:d=4,o=5,b=100:c")), error(RtttlError::Name));
    STIPPLE_CHECK_EQ(error(parse("a-name-that-is-too-long:d=4,o=5,b=100:c")),
                     error(RtttlError::Name));
    // All three defaults are required, as on the clocks' settings page.
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5:c")), error(RtttlError::Defaults));
    STIPPLE_CHECK_EQ(error(parse("x:d=3,o=5,b=100:c")), error(RtttlError::Defaults));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=9,b=100:c")), error(RtttlError::Defaults));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=10:c")), error(RtttlError::Defaults));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,q=1:c")), error(RtttlError::Defaults));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:h")), error(RtttlError::Note));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:c,,d")), error(RtttlError::Note));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:3c")), error(RtttlError::Note));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:c9")), error(RtttlError::Note));
    STIPPLE_CHECK_EQ(error(parse("x:d=4,o=5,b=100:cx")), error(RtttlError::Note));
}

STIPPLE_TEST(Rtttl, ASilentOrEndlessMelodyIsNeverAccepted) {
    // An alarm made only of rests is a silent alarm.
    STIPPLE_CHECK_EQ(error(parse("quiet:d=4,o=5,b=100:p,p,p")), error(RtttlError::Silent));

    // Ten seconds at most: d=1 at 25 bpm is 9.6 s; a second one is too much.
    STIPPLE_CHECK_EQ(error(parse("long:d=1,o=5,b=25:c")), error(RtttlError::None));
    STIPPLE_CHECK_EQ(error(parse("long:d=1,o=5,b=25:c,c")), error(RtttlError::TooLongToPlay));

    std::string many = "many:d=32,o=5,b=900:";
    for (int i = 0; i < 65; ++i) {
        many += i == 0 ? "c" : ",c";
    }
    STIPPLE_CHECK_EQ(error(parse(many)), error(RtttlError::TooManyNotes));

    std::string huge = "huge:d=4,o=5,b=100:c";
    while (huge.size() <= stipple::audio::kMaxRtttlBytes) {
        huge += ",c";
    }
    STIPPLE_CHECK_EQ(error(parse(huge)), error(RtttlError::TooLong));
}

STIPPLE_TEST(MelodyGenerator, EveryNoteLastsExactlyItsSampleCount) {
    Melody melody;
    // 8th notes at 120 bpm = 250 ms = 4000 samples at 16 kHz; the rest too.
    STIPPLE_REQUIRE(parseRtttl("t:d=8,o=5,b=120:c,p,c", melody) == RtttlError::None);

    MelodyGenerator generator(16000);
    generator.start(melody, 100);

    std::vector<std::int16_t> pcm;
    std::int16_t frame[128];
    int guard = 0;
    while (generator.playing() && guard++ < 1000) {
        const int written = generator.fill(frame, 128);
        pcm.insert(pcm.end(), frame, frame + written);
    }
    STIPPLE_CHECK_EQ(static_cast<int>(pcm.size()), 12000);

    // The rest is silence; the notes are not.
    bool restSilent = true;
    for (int i = 4000; i < 8000; ++i) {
        restSilent = restSilent && pcm[static_cast<std::size_t>(i)] == 0;
    }
    STIPPLE_CHECK(restSilent);
    int loudest = 0;
    for (int i = 0; i < 4000; ++i) {
        const int value = pcm[static_cast<std::size_t>(i)];
        loudest = value > loudest ? value : loudest;
    }
    STIPPLE_CHECK(loudest > 8000);  // full level: close to the 9000 peak
}

STIPPLE_TEST(MelodyGenerator, LevelIsAbsoluteAndStopIsImmediate) {
    Melody melody;
    STIPPLE_REQUIRE(parseRtttl("t:d=4,o=5,b=120:a", melody) == RtttlError::None);
    MelodyGenerator generator(16000);
    generator.start(melody, 20);
    std::int16_t frame[512];
    generator.fill(frame, 512);
    int loudest = 0;
    for (const std::int16_t value : frame) {
        loudest = value > loudest ? value : loudest;
    }
    // 20 % of 9000, whatever any volume setting says - there is none here.
    STIPPLE_CHECK(loudest > 1500);
    STIPPLE_CHECK(loudest <= 1800);

    generator.stop();
    STIPPLE_CHECK_FALSE(generator.playing());
    STIPPLE_CHECK_EQ(generator.fill(frame, 512), 0);
    STIPPLE_CHECK_EQ(static_cast<int>(frame[0]), 0);
}

STIPPLE_TEST(SharedSpeaker, RefusesEveryoneElseWhileTheAlarmHoldsIt) {
    SimulatorAudio audio;
    SharedSpeaker speaker;
    speaker.attach(&audio);
    speaker.setNow(1000);

    STIPPLE_CHECK(speaker.playSound("chime"));
    speaker.holdUntil(5000);
    STIPPLE_CHECK(speaker.held());
    STIPPLE_CHECK_FALSE(speaker.playSound("tick"));
    STIPPLE_CHECK_FALSE(speaker.playTone(1000, 60));
    speaker.stop();
    STIPPLE_CHECK_EQ(static_cast<int>(audio.stopCount()), 0);
    STIPPLE_CHECK_EQ(static_cast<int>(audio.requests().size()), 1);

    // Volume still passes through: it never changes the alarm's level.
    speaker.setVolume(10);
    STIPPLE_CHECK_EQ(static_cast<int>(audio.volume()), 10);

    speaker.setNow(5000);
    STIPPLE_CHECK_FALSE(speaker.held());
    STIPPLE_CHECK(speaker.playTone(1000, 60));

    // Nothing holding the shared speaker can play an alarm melody.
    Melody melody;
    STIPPLE_REQUIRE(parseRtttl(stipple::config::kLowMelody, melody) == RtttlError::None);
    STIPPLE_CHECK_FALSE(speaker.playMelody(melody, 100));
}
