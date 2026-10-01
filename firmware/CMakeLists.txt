# STIPPLE core — portable, dependency-free, platform-agnostic.
#
# Everything in this target sits ABOVE the blueprint §53 boundary and must build
# unchanged for host tests, the WASM emulator and the ARM device. Nothing here
# may include a platform header or assume an OS facility.

# The configuration UI is compiled into the binary rather than read from disk.
# A device whose storage has gone wrong is exactly when someone needs its config
# page, so the page must not depend on that storage.
include("${CMAKE_SOURCE_DIR}/cmake/EmbedWebAssets.cmake")

set(STIPPLE_WEB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/WebAssets.generated.cpp")
stipple_embed_web_assets(
    "${CMAKE_CURRENT_SOURCE_DIR}/web"
    "${STIPPLE_WEB_GENERATED}"
    STIPPLE_WEB_SOURCES)

# Re-run configure when a UI source changes, so editing a page is enough to
# rebuild it — without this the generated file would silently go stale.
set_property(DIRECTORY APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${STIPPLE_WEB_SOURCES})

# --- Berry ------------------------------------------------------------------
#
# The scripting language, vendored at a pinned commit (ADR 0022). MIT, ANSI
# C99, under 40 KiB of code.
#
# Its own target rather than part of stipple_core, so core stays
# dependency-free and host-testable and a build without scripting is the same
# core. The sandbox lives in firmware/script/: our berry_conf.h turns off the
# filesystem, the dynamic loader and the bytecode reader, and be_modtab.c is
# an allowlist of the modules a script may import.
file(GLOB STIPPLE_BERRY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/berry/src/*.c")

# be_filelib.c is Berry's file() class. With no filesystem it registers
# nothing, so it is code that can only ever be dead - and on a device with
# 36 MB of RAM, dead code that opens files is worth not shipping.
list(FILTER STIPPLE_BERRY_SOURCES EXCLUDE REGEX "be_filelib\.c$")

add_library(stipple_berry STATIC
    ${STIPPLE_BERRY_SOURCES}
    script/be_port.c
    script/be_modtab.c)

# SYSTEM, so a consumer's warning flags do not apply to Berry's headers.
#
# The same reasoning as the /W0 below, and it has to be said twice because
# they cover different things: that one stops Berry's *sources* being held to
# our flags, this stops its *headers* being held to them inside our own
# targets. berry.h shadows one of its own struct members, which is Berry's
# business and not something to patch in a vendored tree - but with -Wshadow
# -Werror it stopped our code compiling.
#
# Found on the ARM build rather than on the host: MSVC has no equivalent
# warning, so every Windows build of this passed. The same gap as the last
# time something only ever got compiled by one compiler.
target_include_directories(stipple_berry SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/script"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/berry/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/berry/generate")

set_target_properties(stipple_berry PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

# Vendored code does not get this project's warning flags.
#
# Not laziness: -Werror on somebody else's source means their next release
# cannot be taken without editing it, which is how a pinned dependency becomes
# a fork. Our own code in script/ is small enough to read.
if(MSVC)
    target_compile_options(stipple_berry PRIVATE /W0)
else()
    target_compile_options(stipple_berry PRIVATE -w)
endif()

add_library(stipple::berry ALIAS stipple_berry)

# Only the device build compiles BearSSL.
#
# It is 277 C files, and nothing but the TC002 adapter links it - so
# building it for the host tests and the WASM emulator would be a minute
# of CI spent on object files nothing references. The guard also makes the
# ADR 0012 boundary structural rather than a matter of who remembers not
# to link it.
if(STIPPLE_DEVICE_BUILD)
    # --- BearSSL ----------------------------------------------------------------
    #
    # TLS, because the device's own OpenSSL cannot do it.
    #
    # The TC002 carries OpenSSL 1.1.0i, built by OpenWrt in 2018 with every TLS
    # protocol version compiled out - a crypto library with a stub SSL layer.
    # Measured, not assumed: SSL_connect answers NO_PROTOCOLS_AVAILABLE for every
    # protocol floor including none at all, and the per-version methods are absent
    # from its symbol table. So borrowing the platform's TLS is not an option and
    # Stipple has to carry its own.
    #
    # BearSSL rather than mbedTLS or wolfSSL: it is MIT, it is C99 with no
    # dependencies, it allocates nothing on its own - the caller hands it every
    # buffer, which is exactly the constraint §38 already puts on this codebase -
    # and it is about a fifth of the size.
    #
    # **Only the TC002 adapter links this.** stipple_core must stay
    # dependency-free (ADR 0012), and it does: a build without the device adapter
    # is the same core it always was, and the host tests and the WASM emulator
    # never see a byte of it.
    file(GLOB_RECURSE STIPPLE_BEARSSL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/bearssl/src/*.c")

    if(NOT STIPPLE_BEARSSL_SOURCES)
        message(FATAL_ERROR "third_party/bearssl/src is empty - see THIRD_PARTY_NOTICES.md")
    endif()

    add_library(stipple_bearssl STATIC ${STIPPLE_BEARSSL_SOURCES})

    # SYSTEM for the same reason Berry's headers are: a consumer's warning flags
    # are ours to satisfy, not a vendored tree's.
    target_include_directories(stipple_bearssl SYSTEM PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/bearssl/inc")
    target_include_directories(stipple_bearssl PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/bearssl/src")

    set_target_properties(stipple_bearssl PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        # Every static library that ends up inside libstipple.so has to be
        # position-independent, and CMake does not infer that for a STATIC target.
        #
        # Without it the device refuses the library at load time with
        #
        #     unexpected reloc type 0x03
        #
        # which is R_ARM_REL32 - a relocation that cannot appear in a shared
        # object. The shim then falls back to the stock clock, so the symptom is
        # a device that looks fine and is not running Stipple at all.
        #
        # The other vendored and internal targets below get the same property.
        # They happen to work without it today, purely because none of them emits
        # that relocation; that is luck, not design, and the next large const
        # table would end it.
        POSITION_INDEPENDENT_CODE ON)

    # Vendored code does not get this project's warning flags. -Werror on somebody
    # else's source means their next release cannot be taken without editing it,
    # which is how a pinned dependency becomes a fork.
    if(MSVC)
        target_compile_options(stipple_bearssl PRIVATE /W0)
    else()
        target_compile_options(stipple_bearssl PRIVATE -w)
    endif()

    add_library(stipple::bearssl ALIAS stipple_bearssl)
endif()


# The bindings: Berry on one side, Canvas on the other.
#
# Separate from stipple_core for the same reason Berry is - core must stay
# buildable and testable without a scripting language in it.
add_library(stipple_script STATIC
    src/script/ScriptHost.cpp
    src/script/ScriptStore.cpp)

target_include_directories(stipple_script PUBLIC include)
target_link_libraries(stipple_script PUBLIC stipple_core PRIVATE stipple_berry stipple_warnings)
set_target_properties(stipple_script PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(stipple::script ALIAS stipple_script)

add_library(stipple_core
    ${STIPPLE_WEB_GENERATED}
    src/web/StaticFiles.cpp
    src/graphics/Canvas.cpp
    src/demo/TestPattern.cpp
    src/text/Font5x7.cpp
    src/text/Text.cpp
    src/text/Scroll.cpp
    src/notify/Notifications.cpp
    src/input/InputMapper.cpp
    src/input/Navigator.cpp
    src/input/Rescue.cpp
    src/input/SetupHold.cpp
    src/json/Json.cpp
    src/scene/Scene.cpp
    src/audio/Melody.cpp
    src/audio/Tone.cpp
    src/audio/Sound.cpp
    src/time/Timezone.cpp
    src/net/DhcpClient.cpp
    src/net/HttpFetch.cpp
    src/net/ScriptFetcher.cpp
    src/net/DnsMessage.cpp
    src/net/SntpMessage.cpp
    src/core/Base64.cpp
    src/core/Checksum.cpp
    src/core/Md5.cpp
    src/core/Sha1.cpp
    src/core/Sha256.cpp
    src/core/Log.cpp
    src/core/Rgb.cpp
    src/render/FrameScheduler.cpp
    src/render/Overlay.cpp
    src/render/Transition.cpp
    src/asset/IconStore.cpp
    src/apps/BatteryApp.cpp
    src/apps/ClockApp.cpp
    src/apps/GlucoseApp.cpp
    src/apps/GlucoseFacePlan.cpp
    src/apps/GlucoseCloud.cpp
    src/apps/GlucoseFont.cpp
    src/apps/GlucoseAlarm.cpp
    src/apps/GlucoseSource.cpp
    src/apps/StopwatchApp.cpp
    src/apps/VisualizerApp.cpp
    src/apps/Unavailable.cpp
    src/script/ScriptConfig.cpp
    src/script/ScriptMqtt.cpp
    src/script/ScriptRunner.cpp
    src/apps/SplashScreen.cpp
    src/host/ApplicationHost.cpp
    src/app/AppRegistry.cpp
    src/app/Carousel.cpp
    src/config/Config.cpp
    src/config/GlucoseAlarmSettings.cpp
    src/config/GlucoseFaceSettings.cpp
    src/config/GlucoseSourceSettings.cpp
    src/api/Http.cpp
    src/api/JsonWriter.cpp
    src/api/ApiServer.cpp
    src/api/BasicAuth.cpp
    src/mqtt/MqttBridge.cpp
    src/mqtt/ScriptGateway.cpp
    src/mqtt/MqttService.cpp
)

target_include_directories(stipple_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(stipple_core PUBLIC stipple_warnings)

set_target_properties(stipple_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(stipple::core ALIAS stipple_core)

# PNG encoding is a host/emulator convenience (golden-image failure snapshots,
# emulator screenshots). It is deliberately NOT part of stipple_core so it can
# never be linked into the memory-constrained device build by accident - and on
# a device build it is not compiled at all, so "by accident" needs a deliberate
# edit here first.
if(NOT STIPPLE_DEVICE_BUILD)
add_library(stipple_imageio
    imageio/src/Png.cpp
)

target_include_directories(stipple_imageio
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/imageio/include")

target_link_libraries(stipple_imageio PUBLIC stipple_core)

add_library(stipple::imageio ALIAS stipple_imageio)
endif()

# The simulator adapter — the non-device half of the §53 boundary.
#
# A separate target on purpose: stipple_core must never depend on an adapter, and
# keeping them apart is what makes that violation a link error rather than a
# code review comment. The TC002 adapter becomes its sibling in Phase 7.
add_library(stipple_platform_simulator
    src/platform/simulator/SimulatorHttpClient.cpp
    src/platform/simulator/SimulatorPlatform.cpp
)

target_link_libraries(stipple_platform_simulator PUBLIC stipple_core)

add_library(stipple::platform_simulator ALIAS stipple_platform_simulator)

# The TC002 adapter — the device half of the §53 boundary, and the sibling the
# comment above promised.
#
# Device-only because it is POSIX: it opens /dev/spidev0.0 and issues ioctls,
# which neither the host test runner nor a browser can do. stipple_core must
# never depend on it, and keeping them separate targets is what makes that
# violation a link error rather than a review comment.
if(STIPPLE_DEVICE_BUILD)
    add_library(stipple_platform_tc002
        src/platform/tc002/Tc002Audio.cpp
        src/platform/tc002/Tc002Display.cpp
        src/platform/tc002/Tc002Hotspot.cpp
        src/platform/tc002/Tc002Dhcp.cpp
        src/platform/tc002/Tc002Sntp.cpp
        src/platform/tc002/Tc002HttpServer.cpp
        src/platform/tc002/Tc002Input.cpp
        src/platform/tc002/Tc002Mcu.cpp
        src/platform/tc002/Tc002HttpClient.cpp
        src/platform/tc002/Tc002Tls.cpp
        src/platform/tc002/Tc002MqttClient.cpp
        src/platform/tc002/Tc002Platform.cpp
        src/platform/tc002/Tc002Upgrade.cpp
        src/platform/tc002/WpaControl.cpp
    )

    set_target_properties(stipple_platform_tc002 PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # BearSSL is PRIVATE: it is how this adapter does TLS, and nothing above
    # the §53 boundary should be able to reach it by accident.
    target_link_libraries(stipple_platform_tc002
        PUBLIC stipple_core stipple_warnings ${CMAKE_DL_LIBS}
        PRIVATE stipple_bearssl)

    # PUBLIC, so it reaches everything that links this library.
    #
    # Tc002Sntp resolves a hostname on a worker thread, so the whole platform
    # adapter now needs pthread - and at the glibc this device runs,
    # pthread_create still lives in libpthread rather than libc. It was first
    # added to stipple_startup alone, which linked fine and then failed on CI
    # at stipple_device: same library, different consumer, no -pthread.
    #
    # Putting it on the library rather than on each of the five targets that
    # link it means the next tool to be added cannot miss it.
    target_compile_options(stipple_platform_tc002 PUBLIC -pthread)
    target_link_options(stipple_platform_tc002 PUBLIC -pthread)

    add_library(stipple::platform_tc002 ALIAS stipple_platform_tc002)
endif()

# Smoke test for the target architecture. Small enough to push over ADB, and it
# answers the questions that must be settled before anything is flashed: does the
# core run on this CPU at all, and what does the binary actually depend on.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_device_smoke tools/device_smoke/main.cpp)
    target_link_libraries(stipple_device_smoke PRIVATE
        stipple_core
        stipple_platform_simulator
        stipple_warnings)

    # Statically linked, deliberately.
    #
    # A dynamically linked Debian 12 binary requires GLIBC_2.34 purely for
    # __libc_start_main, because that symbol was re-versioned when glibc merged
    # libpthread into libc. The TC002 is an embedded SigmaStar device whose glibc
    # is a §46 unknown and is very unlikely to be that new, so a dynamic binary
    # would fail to start with an error naming a symbol rather than the real
    # cause.
    #
    # Static linking removes the entire question: no interpreter, no versioned
    # symbols, nothing to be too old. It costs binary size, which the 8 MiB res
    # partition can afford far more easily than it can afford a bring-up spent
    # bisecting toolchains.
    #
    # The one thing this gives up is NSS - getaddrinfo cannot dlopen its modules
    # in a static binary. Nothing in core resolves a hostname; when the MQTT
    # transport arrives in Phase 7 and does, that is the moment to revisit this,
    # and the probe will have reported the real glibc version by then.
    target_link_options(stipple_device_smoke PRIVATE -static)
endif()

# Asks the vendor HAL what it offers without calling into it. Dynamically
# linked on purpose - it must dlopen libzkhw.so, which a static binary cannot
# do - so it needs the bullseye toolchain, whose glibc predates the
# __libc_start_main re-versioning. See tooling/cross/Containerfile.bullseye.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_hal_probe tools/hal_probe/main.cpp)
    target_link_libraries(stipple_hal_probe PRIVATE ${CMAKE_DL_LIBS} stipple_warnings)
endif()

# Asks the SigmaStar audio-out library what it is configured for. Read-only:
# it sets nothing and plays nothing. Dynamic for the same reason as the HAL
# probe - it dlopens a vendor library, which a static binary cannot do - so it
# needs the bullseye toolchain.
#
# stipple_warnings is deliberately not linked: it carries -Wold-style-cast and
# friends, which GCC reports as "valid for C++ but not for C", and the device
# preset turns warnings into errors.
if(STIPPLE_DEVICE_BUILD)
    enable_language(C)
    add_executable(stipple_audio_probe tools/audio_probe/main.c)
    target_compile_options(stipple_audio_probe PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    target_link_libraries(stipple_audio_probe PRIVATE ${CMAKE_DL_LIBS})
endif()

# Whether the vendor's own player can play a file, and which EMediaType is
# audio. Dynamic for the same reason as the others - it dlopens
# libzkmedia.so - and it makes a noise, so it is not read-only and is not part
# of tooling/probe.
#
# The finding that made it worth writing: libzkmedia exports
# PlayerFactory::getInstance and ::create, so the object is allocated by code
# that knows its size. The "C++ class of unknown size" objection recorded in
# the findings doc does not apply through a factory.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_player_probe tools/player_probe/main.cpp)
    target_link_libraries(stipple_player_probe PRIVATE ${CMAKE_DL_LIBS} stipple_warnings)
endif()

# The last unknown in the display protocol: channel order.
#
# The spidev capture settled the device node, the SPI mode, the 3072-byte frame
# and the 64-column stride, but every lit pixel in it was white - and white is
# identical under RGB, GRB and BGR. This writes three bands with deliberately
# unequal channels so a person can look at the panel and say which is which.
# It is the one question no amount of further capture can answer.
#
# Plain C, and the only C in the tree, so the language is enabled here rather
# than in the top-level project() - a host build has no reason to gain a C
# compiler requirement for a file it never compiles.
#
# Static for the same reason as the smoke test, and it costs nothing here: this
# talks to spidev through ioctl and write, and dlopens nothing.
#
# stipple_warnings is deliberately NOT linked. It carries -Wold-style-cast and
# friends, which GCC reports as "valid for C++ but not for C" - a warning that
# the device preset's warnings-as-errors would turn into a build failure.
# STIPPLE itself, on the device.
#
# Runs ApplicationHost - the same startup, loop, boot record and safe-mode
# fallback the emulator and the host tests run - over the TC002 platform
# adapter. This is the firmware; stipple_device_panel below is the bring-up demo
# that preceded it and stays because it isolates the display from everything
# else when something goes wrong.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_device tools/device/main.cpp)
    target_link_libraries(stipple_device PRIVATE
        stipple_core
        stipple_platform_tc002
        stipple_script
        stipple_warnings)

    # The same firmware, as the library the TC002 framework loads.
    #
    # /bin/zkgui dlopens whatever /res/etc/EasyUI.cfg names and STIPPLE takes
    # the process in a static constructor (ADR 0021). Built from the very
    # same translation unit as the executable, with one define, because a
    # device build that took a different path from the one tested over ADB
    # would be a second firmware wearing the first one's tests.
    #
    # Named libstipple.so rather than libzkgui.so on purpose: it does not
    # replace the vendor application, it is pointed at instead of it, and the
    # vendor's own library stays where it is.
    add_library(stipple_startup SHARED tools/device/main.cpp)
    set_target_properties(stipple_startup PROPERTIES OUTPUT_NAME stipple)
    target_compile_definitions(stipple_startup PRIVATE STIPPLE_AS_LIBRARY)
    target_link_libraries(stipple_startup PRIVATE
        stipple_core
        stipple_platform_tc002
        stipple_script
        stipple_warnings)

    # The C++ runtime travels with us. The device ships libstdc++.so.6.0.26,
    # whose newest symbol set is GLIBCXX_3.4.26 - GCC 9. Every toolchain in
    # tooling/cross/ is newer than that: bookworm emits GLIBCXX_3.4.30 and
    # bullseye 3.4.28, so *neither* container can produce a library this
    # device can dlopen if it links libstdc++ dynamically.
    #
    # Measured the hard way. A build that flashed and verified perfectly
    # failed at dlopen with
    #
    #     /lib/libstdc++.so.6: version `GLIBCXX_3.4.29' not found
    #
    # and the only reason that was diagnosable is that the shim fell back to
    # the stock clock and left the device on the network.
    #
    # Static libgcc too, for the unwinder: a mixed pair is worse than either.
    target_link_options(stipple_startup PRIVATE
        -static-libstdc++
        -static-libgcc)

    # -pthread comes from stipple_platform_tc002, which needs it for the SNTP
    # resolver thread and declares it PUBLIC. Worth knowing why it matters
    # here: libstdc++ reaches pthread through *weak* symbols, so without it
    # the link succeeds, the library loads, and std::thread fails at runtime.
    # The clock stayed at 1970 with no error anywhere.

    # Dynamic, because the speaker is only reachable through a vendor library.
    #
    # Static linking was the right first answer: it removed every question
    # about which glibc the device carries, and those questions cost days
    # elsewhere. They are now answered - the device has 2.30, and the bullseye
    # toolchain in tooling/cross/ produces executables it accepts.
    #
    # What static cannot do is dlopen, and /lib/libmi_ao.so is the only route
    # to the speaker: there is no ALSA on this device and the vendor
    # application reaches it the same way. A static binary would mean a device
    # that can never make a sound.
    #
    # The cost is that stipple_device now depends on libraries that live on the
    # device. That is a real dependency and it is on the device's own runtime,
    # which is present on every unit that can run the stock application at all.
    if(STIPPLE_DEVICE_STATIC)
        target_link_options(stipple_device PRIVATE -static)
    endif()
endif()

# STIPPLE core rendering on the real panel.
#
# The first thing that puts *our* renderer on the device rather than bytes
# written by hand against the wire: Framebuffer, Canvas and demo::drawTestPattern
# are the same code the emulator and the golden tests run, with only the display
# adapter swapped. Static for the same reason as the smoke test.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_device_panel tools/device_panel/main.cpp)
    target_link_libraries(stipple_device_panel PRIVATE
        stipple_core
        stipple_platform_tc002
        stipple_warnings)
    target_link_options(stipple_device_panel PRIVATE -static)
endif()

if(STIPPLE_DEVICE_BUILD)
    enable_language(C)

    add_executable(stipple_panel_test tools/panel_test/main.c)
    target_compile_options(stipple_panel_test PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    target_link_options(stipple_panel_test PRIVATE -static)

    # Prints what the physical controls actually send. /proc says event67
    # declares four keys; the device tree may well name more than the case has.
    # Safe to run alongside zkgui - evdev delivers to every reader.
    add_executable(stipple_input_probe tools/input_probe/main.c)
    target_compile_options(stipple_input_probe PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    target_link_options(stipple_input_probe PRIVATE -static)

    # Reads the MCU link. The only source of battery state on this device -
    # there is no power_supply class, no hwmon and no IIO.
    add_executable(stipple_mcu_probe tools/mcu_probe/main.c)
    target_compile_options(stipple_mcu_probe PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    target_link_options(stipple_mcu_probe PRIVATE -static)
endif()

# Brings the access point up, waits, and puts the network back on its own.
#
# A separate tool rather than a flag on the firmware, deliberately: the
# firmware's trigger is a *stored* setting, and a device that boots into
# hotspot mode because a test left a flag behind is the failure this is meant
# to avoid.
if(STIPPLE_DEVICE_BUILD)
    add_executable(stipple_hotspot_test tools/hotspot_test/main.cpp)
    target_link_libraries(stipple_hotspot_test PRIVATE
        stipple_platform_tc002
        stipple_warnings)
endif()

# Host-only frame previewer. Renders through the simulator adapter and writes
# PNGs, so output can be inspected without an Emscripten toolchain.
if(NOT EMSCRIPTEN AND NOT STIPPLE_DEVICE_BUILD)
    add_executable(stipple_preview tools/preview/main.cpp)
    target_link_libraries(stipple_preview PRIVATE
        stipple_core
        stipple_imageio
        stipple_platform_simulator
        stipple_warnings)
endif()
