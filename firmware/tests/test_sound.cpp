// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sound catalogue, and the player that walks through one.
//
// These exist because the catalogue used to be an if-chain inside
// `Tc002Audio` with a second and different one in the simulator, so "what
// does alert sound like" had two answers and only one of them was the
// hardware's. It is core code now, which means it can be checked without a
// speaker in the room - and the timing assertions below are the part that
// could not be checked at all before.
#include "stipple/audio/Sound.h"

#include <cstdint>
#include <string>
#include <vector>

#include "support/TestFramework.h"

using stipple::audio::MelodyPlayer;
using stipple::audio::Sound;
using stipple::audio::SoundLibrary;
using stipple::audio::ToneStep;

namespace {

/// Samples at 16 kHz for a given number of milliseconds.
int samplesFor(int millis) { return (millis * 16000) / 1000; }

/// Drain a player in frame-sized bites, the way the device does, and report
/// how many samples came out in total.
///
/// Frame-sized on purpose: pulling the whole sound in one call would not
/// exercise a step boundary landing in the middle of a frame, which is the
/// case that actually breaks.
int drain(MelodyPlayer& player, int frameSize = 128, int maxFrames = 20000) {
    std::vector<std::int16_t> frame(static_cast<std::size_t>(frameSize));
    int total = 0;
    for (int i = 0; i < maxFrames && player.playing(); ++i) {
        const int got = player.fill(frame.data(), frameSize);
        total += got;
        if (got == 0 && !player.playing()) {
            break;
        }
    }
    return total;
}

}  // namespace

STIPPLE_TEST(SoundLibrary, TheOriginalFiveSurvived) {
    // These names are in shipped configuration, in the notification `sound`
    // field, in the web UI's dropdown and in docs/scripting.md. Losing one
    // would silently stop a configured device making a noise.
    for (const char* name : {"beep", "chime", "alert", "tick", "tock"}) {
        STIPPLE_CHECK(SoundLibrary::find(name) != nullptr);
    }
}

STIPPLE_TEST(SoundLibrary, AnUnknownNameIsRefusedRatherThanSubstituted) {
    STIPPLE_CHECK(SoundLibrary::find("trumpet") == nullptr);
    STIPPLE_CHECK(SoundLibrary::find("") == nullptr);

    // "none" is deliberately not a sound. It is a valid *setting* - a clock in
    // a bedroom should be able to say nothing - and the two questions have
    // different answers on purpose.
    STIPPLE_CHECK(SoundLibrary::find("none") == nullptr);
    STIPPLE_CHECK(SoundLibrary::isValidSetting("none"));
    STIPPLE_CHECK(SoundLibrary::isValidSetting("chime"));
    STIPPLE_CHECK_FALSE(SoundLibrary::isValidSetting("trumpet"));
}

STIPPLE_TEST(SoundLibrary, EveryEntryIsWellFormed) {
    std::size_t count = 0;
    const Sound* sounds = SoundLibrary::all(count);
    STIPPLE_REQUIRE(sounds != nullptr);
    STIPPLE_CHECK(count >= 5);

    for (std::size_t i = 0; i < count; ++i) {
        const Sound& sound = sounds[i];

        // A nameless sound could never be asked for, and one claiming more
        // steps than it has storage for would read past its own array.
        STIPPLE_CHECK(!sound.name.empty());
        STIPPLE_CHECK(sound.stepCount > 0);
        STIPPLE_CHECK(sound.stepCount <= Sound::kMaxSteps);

        // find() must reach every entry by its own name, or the catalogue has
        // a row nothing can select.
        STIPPLE_CHECK(SoundLibrary::find(sound.name) == &sound);

        // Nothing on a clock should hold the speaker for a second and a half.
        STIPPLE_CHECK(sound.durationMillis() > 0);
        STIPPLE_CHECK(sound.durationMillis() <= 1500);

        for (std::size_t s = 0; s < sound.stepCount; ++s) {
            const ToneStep& step = sound.steps[s];
            STIPPLE_CHECK(step.durationMillis > 0);
            STIPPLE_CHECK(step.frequencyHz >= 0);
            STIPPLE_CHECK(step.gainPermille >= 0);
            STIPPLE_CHECK(step.gainPermille <= 1000);
        }
    }
}

STIPPLE_TEST(SoundLibrary, NamesAreUnique) {
    // A duplicate would make the second entry unreachable, and it would be
    // unreachable silently.
    std::size_t count = 0;
    const Sound* sounds = SoundLibrary::all(count);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            STIPPLE_CHECK(sounds[i].name != sounds[j].name);
        }
    }
}

STIPPLE_TEST(MelodyPlayer, ASingleNoteLastsItsDuration) {
    MelodyPlayer player;
    player.startTone(880, 100);
    STIPPLE_CHECK(player.playing());

    const int produced = drain(player);
    STIPPLE_CHECK_FALSE(player.playing());

    // Within one frame, because a note ends where it ends rather than on a
    // frame boundary.
    const int expected = samplesFor(100);
    STIPPLE_CHECK(produced >= expected - 128);
    STIPPLE_CHECK(produced <= expected + 128);
}

STIPPLE_TEST(MelodyPlayer, ASequenceLastsTheSumOfItsSteps) {
    // The whole reason this class exists: `chime` is two notes now, and a
    // player that stopped after the first would have sounded like the old
    // one-note version and nobody would have noticed from the outside.
    const Sound* alert = SoundLibrary::find("alert");
    STIPPLE_REQUIRE(alert != nullptr);
    STIPPLE_CHECK(alert->stepCount > 1);

    MelodyPlayer player;
    player.start(*alert);
    const int produced = drain(player);

    const int expected = samplesFor(alert->durationMillis());
    STIPPLE_CHECK(produced >= expected - 256);
    STIPPLE_CHECK(produced <= expected + 256);
}

STIPPLE_TEST(MelodyPlayer, ARestIsSilenceAndNotAShortRead) {
    // A rest must write zeroes rather than return short. Returning short
    // would let the driver run dry in the middle of a sound, and on a
    // six-frame buffer that is audible as a stutter rather than a gap.
    Sound gap = {};
    gap.name = "test";
    gap.steps[0] = ToneStep{0, 50, 0};  // 50 ms of nothing
    gap.stepCount = 1;

    MelodyPlayer player;
    player.start(gap);
    STIPPLE_CHECK(player.playing());

    std::int16_t frame[128] = {};
    for (std::size_t i = 0; i < 128; ++i) {
        frame[i] = 999;  // poison, so silence has to be written rather than assumed
    }

    const int got = player.fill(frame, 128);
    STIPPLE_CHECK_EQ(got, 128);
    for (std::size_t i = 0; i < 128; ++i) {
        STIPPLE_CHECK_EQ(frame[i], static_cast<std::int16_t>(0));
    }
}

STIPPLE_TEST(MelodyPlayer, StartingAgainReplacesRatherThanQueues) {
    // Same rule as ToneGenerator::start, and it matters more here because a
    // sequence is long enough to overlap. A device that queued alerts would
    // fall behind the thing it is alerting about.
    const Sound* alarm = SoundLibrary::find("alarm");
    STIPPLE_REQUIRE(alarm != nullptr);

    MelodyPlayer player;
    player.start(*alarm);

    std::int16_t frame[128] = {};
    player.fill(frame, 128);

    player.startTone(880, 20);
    const int produced = drain(player);

    // What is left is the short tone, not the rest of the alarm.
    STIPPLE_CHECK(produced <= samplesFor(20) + 128);
}

STIPPLE_TEST(MelodyPlayer, StopIsImmediate) {
    const Sound* alarm = SoundLibrary::find("alarm");
    STIPPLE_REQUIRE(alarm != nullptr);

    MelodyPlayer player;
    player.start(*alarm);
    STIPPLE_CHECK(player.playing());

    player.stop();
    STIPPLE_CHECK_FALSE(player.playing());

    std::int16_t frame[128] = {};
    STIPPLE_CHECK_EQ(player.fill(frame, 128), 0);
}

STIPPLE_TEST(MelodyPlayer, AZeroLengthStepIsSkippedRatherThanHanging) {
    // Guards the loop in beginStep(). A step with no duration is not a sound,
    // and treating it as one would spin.
    Sound odd = {};
    odd.name = "test";
    odd.steps[0] = ToneStep{440, 0, 0};
    odd.steps[1] = ToneStep{880, 20, 0};
    odd.stepCount = 2;

    MelodyPlayer player;
    player.start(odd);
    const int produced = drain(player);
    STIPPLE_CHECK_FALSE(player.playing());
    STIPPLE_CHECK(produced > 0);
    STIPPLE_CHECK(produced <= samplesFor(20) + 128);
}

STIPPLE_TEST(MelodyPlayer, QuietNotesAreQuieterThanLoudOnes) {
    // The per-note gain is what makes a clock tick liveable once a second, so
    // it is worth one assertion that it does something rather than being
    // carried along as an unused field.
    const auto peak = [](MelodyPlayer& player) {
        std::int16_t frame[256] = {};
        int highest = 0;
        while (player.playing()) {
            const int got = player.fill(frame, 256);
            if (got == 0) {
                break;
            }
            for (int i = 0; i < got; ++i) {
                const int magnitude = frame[i] < 0 ? -frame[i] : frame[i];
                if (magnitude > highest) {
                    highest = magnitude;
                }
            }
        }
        return highest;
    };

    MelodyPlayer loud;
    loud.startTone(880, 120, 1000);
    const int loudPeak = peak(loud);

    MelodyPlayer soft;
    soft.startTone(880, 120, 200);
    const int softPeak = peak(soft);

    STIPPLE_CHECK(loudPeak > 0);
    STIPPLE_CHECK(softPeak > 0);
    STIPPLE_CHECK(softPeak < loudPeak);
}
