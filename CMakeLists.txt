# 3.18, not 3.21.
#
# CMakePresets.json still needs 3.21 and every host build uses it, but the
# device build cannot: stipple_device has to be linked by the bullseye
# toolchain to reach the speaker, and bullseye ships cmake 3.18. Raising the
# floor here would have meant either no audio or an unpinned toolchain, and
# nothing in this file uses anything newer.
cmake_minimum_required(VERSION 3.18)

project(stipple
    VERSION 0.2.9
    DESCRIPTION "Open pixel firmware for the Ulanzi TC002"
    # C as well as C++: Berry is ANSI C99 (ADR 0022), and the probe tools
    # under firmware/tools/ have always been C.
    LANGUAGES CXX C)

# STIPPLE core is portable C++17 with no external dependencies. It must compile
# identically for the host test runner, the Emscripten emulator and (later) the
# arm-linux-gnueabihf device target. See docs/adr/0003-framebuffer-renderer.md.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(STIPPLE_BUILD_TESTS    "Build the host unit and golden-image tests" ON)
option(STIPPLE_BUILD_EMULATOR "Build the Emscripten browser emulator"      OFF)
option(STIPPLE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"      OFF)

# A device build carries only what the device needs. The PNG encoder and the
# preview tool are host conveniences, and CLAUDE.md is explicit that imageio must
# never be linked into the memory-constrained device build - so make that a
# build-system fact rather than a habit.
option(STIPPLE_DEVICE_BUILD   "Cross build for the TC002: core only, no host tools" OFF)

# Kept as an escape hatch rather than deleted. A static stipple_device cannot
# reach the speaker, but it also cannot be broken by anything on the device's
# filesystem - which is the right trade while bringing up an unfamiliar unit.
option(STIPPLE_DEVICE_STATIC "Link stipple_device statically; disables audio" OFF)

# Included after the options above: StippleWarnings reads them at include time.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(StippleWarnings)

if(EMSCRIPTEN)
    # The emulator target owns its own link flags; tests cannot run under WASM.
    set(STIPPLE_BUILD_TESTS OFF)
    set(STIPPLE_BUILD_EMULATOR ON)
endif()

add_subdirectory(firmware)

if(STIPPLE_BUILD_EMULATOR)
    add_subdirectory(simulator/web)
endif()

if(STIPPLE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(firmware/tests)
endif()

message(STATUS "STIPPLE ${PROJECT_VERSION}")
message(STATUS "  device build : ${STIPPLE_DEVICE_BUILD}")
message(STATUS "  tests        : ${STIPPLE_BUILD_TESTS}")
message(STATUS "  emulator     : ${STIPPLE_BUILD_EMULATOR}")
message(STATUS "  warn-as-error: ${STIPPLE_WARNINGS_AS_ERRORS}")
