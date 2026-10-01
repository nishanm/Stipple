// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002Audio.h"

#include <dlfcn.h>

#include <cstring>

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

/// libmi_ao.so does not name its own dependencies - it expects the host process
/// to have loaded the SigmaStar support libraries already, which zkgui does.
/// Loading them RTLD_GLOBAL first puts their symbols where it can find them;
/// without this it fails to load at all, on CamOsGetTimeOfDay.
const char* const kSupport[] = {
    "/lib/libcam_os_wrapper.so",
    "/lib/libcam_fs_wrapper.so",
    "/lib/libmi_common.so",
    "/lib/libmi_sys.so",
};

const char* const kLibrary = "/lib/libmi_ao.so";

/// MI_AUDIO_Attr_t, exactly as the vendor application fills it.
struct AudioAttr {
    std::uint32_t sampleRate;
    std::uint32_t bitWidth;       ///< 0 = 16-bit
    std::uint32_t workMode;       ///< 0 = I2S master
    std::uint32_t soundMode;      ///< 0 = mono
    std::uint32_t frameNum;
    std::uint32_t pointsPerFrame;
    std::uint32_t codecChnCnt;
    std::uint32_t chnCnt;
    std::uint32_t reserved;
    /// Slack, zeroed. The capture showed the fields above; anything the real
    /// struct carries past them takes whatever the driver treats as default,
    /// which is better than a number chosen by somebody guessing.
    std::uint32_t slack[8];
};

/// Device and channel. Both zero on this hardware, and named rather than
/// written as bare literals at every call site.
constexpr int kDevice = 0;
constexpr int kChannel = 0;

}  // namespace

Tc002Audio::~Tc002Audio() { close(); }

bool Tc002Audio::open() {
    close();

    for (const char* support : kSupport) {
        // Failures are not fatal on their own: a device that has already loaded
        // one of these gets NULL back and carries on fine. The load of
        // libmi_ao.so below is the check that matters.
        ::dlopen(support, RTLD_NOW | RTLD_GLOBAL);
    }

    library_ = ::dlopen(kLibrary, RTLD_NOW | RTLD_GLOBAL);
    if (library_ == nullptr) {
        return false;
    }

    setPubAttr_ = reinterpret_cast<int (*)(int, const void*)>(
        ::dlsym(library_, "MI_AO_SetPubAttr"));
    enable_ = reinterpret_cast<int (*)(int)>(::dlsym(library_, "MI_AO_Enable"));
    enableChn_ = reinterpret_cast<int (*)(int, int)>(::dlsym(library_, "MI_AO_EnableChn"));
    sendFrame_ = reinterpret_cast<int (*)(int, int, const void*, int)>(
        ::dlsym(library_, "MI_AO_SendFrame"));
    clearChnBuf_ = reinterpret_cast<int (*)(int, int)>(::dlsym(library_, "MI_AO_ClearChnBuf"));
    disableChn_ = reinterpret_cast<int (*)(int, int)>(::dlsym(library_, "MI_AO_DisableChn"));
    disable_ = reinterpret_cast<int (*)(int)>(::dlsym(library_, "MI_AO_Disable"));

    if (setPubAttr_ == nullptr || enable_ == nullptr || enableChn_ == nullptr ||
        sendFrame_ == nullptr) {
        close();
        return false;
    }

    AudioAttr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.sampleRate = kSampleRate;
    attr.bitWidth = 0;
    attr.workMode = 0;
    attr.soundMode = 0;
    attr.frameNum = 6;
    attr.pointsPerFrame = kPointsPerFrame;
    attr.codecChnCnt = 0;
    attr.chnCnt = 1;

    if (setPubAttr_(kDevice, &attr) != 0 || enable_(kDevice) != 0 ||
        enableChn_(kDevice, kChannel) != 0) {
        close();
        return false;
    }

    channelEnabled_ = true;
    tone_.setVolumePercent((static_cast<int>(volume_) * 100) / 255);
    return true;
}

void Tc002Audio::close() noexcept {
    if (channelEnabled_) {
        if (disableChn_ != nullptr) {
            disableChn_(kDevice, kChannel);
        }
        if (disable_ != nullptr) {
            disable_(kDevice);
        }
        channelEnabled_ = false;
    }
    if (library_ != nullptr) {
        ::dlclose(library_);
        library_ = nullptr;
    }
    setPubAttr_ = nullptr;
    enable_ = nullptr;
    enableChn_ = nullptr;
    sendFrame_ = nullptr;
    clearChnBuf_ = nullptr;
    disableChn_ = nullptr;
    disable_ = nullptr;
    tone_.stop();
    melody_.stop();
}

bool Tc002Audio::sendFrame() {
    if (melody_.playing()) {
        melody_.fill(samples_, kPointsPerFrame);
    } else {
        tone_.fill(samples_, kPointsPerFrame);
    }

    std::memset(frame_, 0, sizeof(frame_));
    // eBitwidth and eSoundmode are the first two words and both zero here,
    // which the vendor's own configuration says is 16-bit mono.
    void* data = samples_;
    std::memcpy(frame_ + 8, &data, sizeof(data));
    const std::uint32_t length = sizeof(samples_);
    std::memcpy(frame_ + kFrameLengthOffset, &length, sizeof(length));

    // Zero timeout: hand it over if there is room, and come back next tick if
    // there is not. Anything else would park the render loop on a speaker.
    return sendFrame_(kDevice, kChannel, frame_, 0) == 0;
}

void Tc002Audio::tick() {
    if (!channelEnabled_) {
        return;
    }

    // Bounded per tick as well as by the driver refusing. A frame is 8 ms of
    // sound, so this hands over at most a quarter of a second and returns -
    // enough to stay ahead of the driver's six-frame buffer without ever
    // becoming the reason a frame was late.
    constexpr int kMaxFramesPerTick = 30;
    for (int i = 0; i < kMaxFramesPerTick && (melody_.playing() || tone_.playing()); ++i) {
        if (!sendFrame()) {
            break;
        }
    }
}

bool Tc002Audio::playTone(int frequencyHz, int durationMillis) {
    // An alarm is not cut off by a beep: ToneGenerator replaces what it is
    // playing, so the refusal has to happen here.
    if (!channelEnabled_ || melody_.playing()) {
        return false;
    }
    tone_.start(frequencyHz, durationMillis);
    return tone_.playing();
}

bool Tc002Audio::playSound(std::string_view name) {
    if (!channelEnabled_ || melody_.playing()) {
        return false;
    }

    // A deliberately short list, and tones rather than samples.
    //
    // The device can decode MP3 - libmad.so is present and the vendor
    // application plays files with it - but shipping stored audio is a separate
    // piece of work with its own storage budget. Naming sounds we cannot make
    // would be the same lie as a volume control with no speaker behind it, so
    // an unknown name is refused rather than quietly turned into a beep.
    if (name == "beep") {
        tone_.start(880, 120);
    } else if (name == "chime") {
        tone_.start(1320, 180);
    } else if (name == "alert") {
        tone_.start(660, 400);
    } else if (name == "tick") {
        // Short and quiet, and deliberately not as loud as anything a button
        // does. This plays once a second for as long as the clock is on
        // screen, and the difference between charming and maddening is
        // entirely in how far under the rest of the device it sits.
        tone_.start(2200, 10, 180);
    } else if (name == "tock") {
        // A shade lower, so a second sounds like a second rather than like a
        // repeated blip. Real clocks do this because the escapement is not
        // symmetric; here it is on purpose.
        tone_.start(1800, 10, 180);
    } else {
        return false;
    }
    return tone_.playing();
}

void Tc002Audio::stop() {
    tone_.stop();
    // A script's stop() is about its own beeps; an alarm melody carries on,
    // buffered frames included.
    if (melody_.playing()) {
        return;
    }
    if (channelEnabled_ && clearChnBuf_ != nullptr) {
        // Otherwise "stop" means "stop after whatever is already buffered",
        // which on a six-frame buffer is a noticeable tail.
        clearChnBuf_(kDevice, kChannel);
    }
}

bool Tc002Audio::playMelody(const audio::Melody& melody, int levelPercent) {
    if (!channelEnabled_) {
        return false;
    }
    tone_.stop();
    // Whatever tone was already handed to the driver would play first and
    // push the alarm back by up to its six-frame buffer.
    if (clearChnBuf_ != nullptr) {
        clearChnBuf_(kDevice, kChannel);
    }
    melody_.start(melody, levelPercent);
    return melody_.playing();
}

void Tc002Audio::stopMelody() {
    if (!melody_.playing()) {
        return;
    }
    melody_.stop();
    if (channelEnabled_ && clearChnBuf_ != nullptr) {
        clearChnBuf_(kDevice, kChannel);
    }
}

void Tc002Audio::setVolume(std::uint8_t volume) {
    volume_ = volume;
    tone_.setVolumePercent((static_cast<int>(volume) * 100) / 255);
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
