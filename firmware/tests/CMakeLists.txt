add_executable(stipple_tests
    support/TestFramework.cpp
    support/Golden.cpp
    test_geometry.cpp
    test_rgb.cpp
    test_canvas.cpp
    test_test_pattern.cpp
    test_utf8.cpp
    test_text.cpp
    test_input_mapper.cpp
    test_simulator_platform.cpp
    test_json.cpp
    test_scene.cpp
    test_app.cpp
    test_config.cpp
    test_scroll.cpp
    test_notifications.cpp
    test_api.cpp
    test_firmware_update.cpp
    test_memory_budget.cpp
    test_log.cpp
    test_frame_scheduler.cpp
    test_host.cpp
    test_clock.cpp
    test_glucose.cpp
    test_glucose_alarm.cpp
    test_glucose_alarm_host.cpp
    test_melody.cpp
    test_glucose_source.cpp
    test_glucose_settings.cpp
    test_glucose_faces.cpp
    test_glucose_cloud.cpp
    test_icons.cpp
    test_web.cpp
    test_mqtt.cpp
    test_mqtt_discovery.cpp
    test_rotary_decoder.cpp
    test_mcu_protocol.cpp
    test_navigator.cpp
    test_tone.cpp
    test_timezone.cpp
    test_wireless_stats.cpp
    test_rescue.cpp
    test_setup_hold.cpp
    test_wpa_replies.cpp
    test_dhcp.cpp
    test_dns.cpp
    test_sntp.cpp
    test_splash.cpp
    test_stopwatch.cpp
    test_elf_check.cpp
    test_ipv4_udp.cpp
    test_wpa_commands.cpp
    test_basic_auth.cpp
    test_md5.cpp
    test_sha1.cpp
    test_sha256.cpp
    test_update_image.cpp
    test_script.cpp
    test_script_store.cpp
    test_script_audio.cpp
    test_sound.cpp
    test_script_mqtt.cpp
    test_script_config.cpp
    test_script_input.cpp
    test_http_fetch.cpp
    test_resolver.cpp
    test_shop_scripts.cpp
)

target_include_directories(stipple_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(stipple_tests PRIVATE
    stipple_core
    stipple_imageio
    stipple_platform_simulator
    stipple_script
    stipple_warnings)

# Golden fixtures are read from the source tree, not the build tree, so a failing
# test can write its diff PNGs next to the fixture it disagrees with.
target_compile_definitions(stipple_tests PRIVATE
    STIPPLE_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# The published scripts are read from the source tree for the same reason as
# the golden fixtures: they are the files people will actually download, and a
# copy staged into the build directory is a copy that can go stale.
target_compile_definitions(stipple_tests PRIVATE
    STIPPLE_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../scripts")

# The published scripts, listed for the test at configure time.
#
# They were written out by hand in test_shop_scripts.cpp so that a file going
# missing would fail loudly rather than being silently skipped - which a
# directory scan at runtime would do, reporting "no scripts found, nothing to
# check, all good".
#
# Generating the list gives a stronger property and drops the chore. The page
# and the test now come from the same directory, so the shop cannot list a
# script the suite does not run - which is what the hand-written list was
# guarding, done by construction instead of by vigilance.
#
# What it does not do is notice a deletion: CONFIGURE_DEPENDS re-runs cmake,
# so removing a file removes it from the list and from the page together.
# That is what deleting a script means, and the case worth failing on - an
# empty directory - is caught below.
file(GLOB STIPPLE_SHOP_SCRIPTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/*.be")

if(NOT STIPPLE_SHOP_SCRIPTS)
    message(FATAL_ERROR "no scripts in scripts/ - the shop tests would pass by testing nothing")
endif()

set(STIPPLE_SHOP_SCRIPT_LIST "")
foreach(script IN LISTS STIPPLE_SHOP_SCRIPTS)
    get_filename_component(name "${script}" NAME)
    string(APPEND STIPPLE_SHOP_SCRIPT_LIST "\"${name}\",")
endforeach()

target_compile_definitions(stipple_tests PRIVATE
    STIPPLE_SHOP_SCRIPT_LIST=${STIPPLE_SHOP_SCRIPT_LIST})

if(MSVC)
    # The golden harness reads STIPPLE_UPDATE_GOLDEN / STIPPLE_STRICT_GOLDEN with
    # std::getenv, which MSVC deprecates in favour of the non-portable
    # _dupenv_s. This is host-only test code reading its own env vars, so the
    # portable call stays and the warning is suppressed here rather than in core.
    target_compile_definitions(stipple_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_test(NAME stipple_tests COMMAND stipple_tests)
