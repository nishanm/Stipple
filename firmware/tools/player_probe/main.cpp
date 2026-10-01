// SPDX-License-Identifier: GPL-3.0-or-later
//
// Can the TC002 play an audio *file*, through the vendor's own player?
//
// `Tc002Audio` synthesises PCM and hands it to MI_AO a frame at a time, which
// is how every sound STIPPLE makes is a sequence of notes. The device can
// obviously do more than that - the vendor application plays
// /res/ui/audio/Tip.mp3 - and `docs/research/tc002-platform-findings.md`
// recorded why we had not followed it:
//
//     SoundDevice is a C++ class, so using it through dlsym means allocating
//     storage for an object whose size we do not know. Over-allocating is the
//     usual trick and it usually works; it is also precisely the sort of
//     "usually works" this project has been avoiding.
//
// That objection is answered by the symbol table rather than argued with.
// /lib/libzkmedia.so exports a **factory**:
//
//     _ZN5media13PlayerFactory11getInstanceEv     PlayerFactory::getInstance()
//     _ZN5media13PlayerFactory6createE10EMediaType  ::create(EMediaType)
//     _ZN5media13ZKAudioPlayer4playEPKc             ZKAudioPlayer::play(const char*)
//
// The factory allocates the object, so nothing here has to know how big a
// ZKAudioPlayer is. That is the whole reason this probe is worth writing.
//
// **Two things are genuinely unknown and are what this measures.**
//
// `EMediaType` is an enum whose audio value nobody has seen, so this tries the
// small ones in order and reports which produced a player that would play.
//
// And whether the vendor player can coexist with our MI_AO channel. Both want
// /dev/mi_ao. If it cannot, file playback means closing our channel first and
// reopening it after, which is a design consequence rather than a detail - so
// it is better to find out here than to discover it as a sound that works
// only when the device is otherwise silent.
//
// **This plays a file that is already on the device.** Nothing vendor-derived
// is copied here or shipped anywhere: the point is to prove the mechanism,
// and /res/ui/audio/Tip.mp3 is sitting on the user's own clock already.
//
// It makes a noise, so it is not read-only, and it is not `probe.py` - it is a
// throwaway alongside mcu_probe and input_probe. Run it with the vendor
// application stopped, or the two will fight over the speaker.
//
//     setprop ctl.stop zkswe
//     /tmp/stipple_player_probe /res/ui/audio/Tip.mp3

#include <dlfcn.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

/// The library is opened by soname, the way the vendor application reaches it.
constexpr const char* kLibrary = "libzkmedia.so";

/// What has to be in the process before libzkmedia will load at all.
///
/// This was the probe's first surprise and it is the useful part of the whole
/// exercise. A bare `dlopen("libzkmedia.so")` fails:
///
///     /lib/libzkmedia.so: undefined symbol: _ZTI6Thread
///
/// `libzkmedia` uses a vendor `Thread` class and does not declare where it
/// comes from - its NEEDED list is libmi_*, libmad, libstdc++, libgcc_s and
/// libc, with no mention of EasyUI. `libeasyui.so` is what defines
/// `_ZTI6Thread` and `_ZTV6Thread`.
///
/// Opening *that* fails too, twice over: first on
/// `_ZTVN10__cxxabiv120__si_class_type_infoE` from libstdc++, which this
/// probe had not linked because it uses only <cstdio> and --as-needed
/// dropped it; then on `jpeg_resync_to_restart`. And the reason is that
/// **libeasyui declares only `libgcc_s.so.1` and `libc.so.6`** while actually
/// depending on libstdc++, libjpeg, libpng, freetype, zlib and more. It is
/// not loadable standalone by design - it only ever runs inside `/bin/zkgui`,
/// which links all twenty-five of them, so the vendor never had to declare
/// anything.
///
/// **None of which the real adapter will hit.** STIPPLE runs *inside* that
/// same host, and `/proc/<pid>/maps` on a live unit shows libeasyui,
/// libstdc++, libjpeg and the rest already mapped. So this list is not a set
/// of dependencies STIPPLE acquires; it is this probe catching up with the
/// process its subject normally lives in, taken from that maps output rather
/// than guessed.
///
/// RTLD_GLOBAL on each is load-bearing: a symbol has to enter the global
/// scope for the *next* dlopen to find it.
constexpr const char* kPreloads[] = {
    "libstdc++.so.6",
    // The SigmaStar stack. libeasyui reaches MI_SYS_Mmap without declaring
    // that either, and libzkmedia's own audio path is MI_AO.
    "libmi_common.so",
    "libmi_sys.so",
    "libmi_ao.so",
    "libmi_gfx.so",
    "libmi_panel.so",
    "libmi_disp.so",
    "libz.so.1",
    "libjpeg.so.9",
    "libpng12.so.0",
    "libfreetype.so.6",
    "liblog.so",
    "libcutils.so",
    "libiniparser.so",
    "libcam_os_wrapper.so",
    "libcam_fs_wrapper.so",
    "libts.so",
    "libnanovg.so",
    "libeasyui.so",
};

/// Three passes over the list above.
///
/// Rather than working out a load order by hand: RTLD_GLOBAL makes each
/// success visible to every later attempt, so a library that failed because
/// its own dependency had not loaded yet succeeds on the next pass. Cheap, and
/// it avoids encoding a dependency graph nobody has verified.
constexpr int kPreloadPasses = 3;

/// Mangled names, verbatim from the symbol table. Spelled out rather than
/// derived because a mangling this probe got subtly wrong would look exactly
/// like a symbol the device does not have.
constexpr const char* kGetInstance = "_ZN5media13PlayerFactory11getInstanceEv";
constexpr const char* kCreate = "_ZN5media13PlayerFactory6createE10EMediaType";
constexpr const char* kPlay = "_ZN5media13ZKAudioPlayer4playEPKc";
constexpr const char* kStop = "_ZN5media13ZKAudioPlayer4stopEv";
constexpr const char* kDuration = "_ZN5media13ZKAudioPlayer11getDurationEv";
constexpr const char* kSetVolume = "_ZN5media13ZKAudioPlayer9setVolumeEf";

/// Non-static member functions, called through the pointer the factory
/// returned. `this` is the first argument on ARM's C++ ABI, which is why a
/// plain function pointer can stand in for a method here - the same trick the
/// HAL probe uses, and the reason the factory matters: the object came from
/// code that knows its size.
using GetInstanceFn = void* (*)();
using CreateFn = void* (*)(void*, int);
using PlayFn = int (*)(void*, const char*);
using StopFn = int (*)(void*);
using DurationFn = int (*)(void*);
using SetVolumeFn = int (*)(void*, float);

template <typename Fn>
Fn resolve(void* handle, const char* name) {
    dlerror();
    void* symbol = dlsym(handle, name);
    const char* problem = dlerror();
    if (symbol == nullptr || problem != nullptr) {
        std::printf("  MISSING  %s\n", name);
        return nullptr;
    }
    std::printf("  ok       %s\n", name);
    return reinterpret_cast<Fn>(symbol);
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/res/ui/audio/Tip.mp3";

    std::printf("player_probe: %s\n\n", path);

    // RTLD_GLOBAL on each, so the next one in the chain can see its symbols.
    // See kPreloads for why any of this is necessary.
    //
    // A failure is reported and survived rather than fatal: the list is the
    // real process's library set, not a minimal one, and libzkmedia may well
    // not need all of it. Only libzkmedia itself failing is worth stopping
    // for.
    bool loaded[sizeof(kPreloads) / sizeof(kPreloads[0])] = {};
    for (int pass = 0; pass < kPreloadPasses; ++pass) {
        for (std::size_t i = 0; i < sizeof(loaded) / sizeof(loaded[0]); ++i) {
            if (loaded[i]) {
                continue;
            }
            if (dlopen(kPreloads[i], RTLD_NOW | RTLD_GLOBAL) != nullptr) {
                loaded[i] = true;
                std::printf("  preloaded %s\n", kPreloads[i]);
            } else if (pass == kPreloadPasses - 1) {
                std::printf("  SKIPPED   %s: %s\n", kPreloads[i], dlerror());
            }
        }
    }
    std::printf("\n");

    void* handle = dlopen(kLibrary, RTLD_NOW);
    if (handle == nullptr) {
        std::printf("dlopen(%s) failed: %s\n", kLibrary, dlerror());
        return 1;
    }
    std::printf("%s loaded\n", kLibrary);

    auto getInstance = resolve<GetInstanceFn>(handle, kGetInstance);
    auto create = resolve<CreateFn>(handle, kCreate);
    auto play = resolve<PlayFn>(handle, kPlay);
    auto stop = resolve<StopFn>(handle, kStop);
    auto duration = resolve<DurationFn>(handle, kDuration);
    auto setVolume = resolve<SetVolumeFn>(handle, kSetVolume);

    if (getInstance == nullptr || create == nullptr || play == nullptr) {
        std::printf("\nthe three that matter are not all here; stopping\n");
        dlclose(handle);
        return 1;
    }

    void* factory = getInstance();
    std::printf("\nPlayerFactory::getInstance() -> %p\n", factory);
    if (factory == nullptr) {
        dlclose(handle);
        return 1;
    }

    // EMediaType's audio value is the unknown. Enums here start at zero and
    // there will not be many, so walk the low ones and report what each gives
    // back rather than asserting which is right.
    void* player = nullptr;
    int mediaType = -1;
    for (int candidate = 0; candidate < 6; ++candidate) {
        void* made = create(factory, candidate);
        std::printf("  create(%d) -> %p\n", candidate, made);
        if (made != nullptr && player == nullptr) {
            player = made;
            mediaType = candidate;
        }
    }

    if (player == nullptr) {
        std::printf("\nno EMediaType in 0..5 produced a player\n");
        dlclose(handle);
        return 1;
    }

    std::printf("\nusing EMediaType %d\n", mediaType);

    if (setVolume != nullptr) {
        // Half, so a probe run next to somebody's head is not the loudest
        // thing in the room.
        std::printf("setVolume(0.5) -> %d\n", setVolume(player, 0.5F));
    }

    const int result = play(player, path);
    std::printf("play(\"%s\") -> %d\n", path, result);

    if (duration != nullptr) {
        // Asked after play(), because a player that has not opened a file has
        // nothing to report the length of.
        std::printf("getDuration() -> %d\n", duration(player));
    }

    // Playback is on the player's own thread - libzkmedia exports
    // ZKAudioPlayer::threadLoop - so play() returning does not mean the sound
    // has been made. Wait long enough for a person to hear it, which is the
    // only test that counts here.
    std::printf("\nlistening for 5 seconds...\n");
    for (int i = 0; i < 5; ++i) {
        sleep(1);
        std::printf("  %ds\n", i + 1);
    }

    if (stop != nullptr) {
        std::printf("stop() -> %d\n", stop(player));
    }

    // Deliberately not deleting the player. Its destructor is exported but
    // calling one through dlsym on an object whose layout we do not know is
    // the risk this probe was written to avoid; the process is about to exit
    // and take it with us.
    std::printf("\ndone. If you heard it, file playback works.\n");
    dlclose(handle);
    return 0;
}
