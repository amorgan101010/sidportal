cmake_minimum_required(VERSION 3.20)
project(sidportal LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# The static libraries also link into the VST3 (a shared object).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# reSID (GPL-2.0-or-later), configured headers pre-generated in vendor/resid.
add_library(resid STATIC
    vendor/resid/sid.cc vendor/resid/voice.cc vendor/resid/wave.cc vendor/resid/envelope.cc
    vendor/resid/filter.cc vendor/resid/dac.cc vendor/resid/extfilt.cc vendor/resid/pot.cc
    vendor/resid/version.cc)
target_compile_definitions(resid PRIVATE VERSION="1.0-sidportal")
target_include_directories(resid PUBLIC vendor/resid)
set_target_properties(resid PROPERTIES CXX_STANDARD 11)
target_compile_options(resid PRIVATE -w)

# reSIDfp from libresidfp (GPL-2.0-or-later), configured headers pre-generated.
find_package(Threads REQUIRED)
file(GLOB RESIDFP_SOURCES vendor/residfp/src/*.cpp)
add_library(residfp STATIC ${RESIDFP_SOURCES} vendor/residfp/src/version.cc
    vendor/residfp/src/resample/SincResampler.cpp vendor/residfp/src/residfp/residfp.cpp)
target_include_directories(residfp PUBLIC vendor/residfp/src)
target_compile_definitions(residfp PRIVATE NDEBUG)
if(MSVC)
    # MSVC otherwise reports the legacy __cplusplus value, which siddefs-fp.h
    # uses to reject the compiler even when CMake selects a modern C++ mode.
    target_compile_options(residfp PRIVATE /Zc:__cplusplus)
endif()
target_link_libraries(residfp PUBLIC Threads::Threads)
target_compile_options(residfp PRIVATE -w)

# The board alone, with no host I/O.
add_library(sidportal_core STATIC src/core/hc11.cpp src/core/board.cpp src/core/sid_engine.cpp)
target_include_directories(sidportal_core PUBLIC src/core)
target_link_libraries(sidportal_core PUBLIC resid residfp)
if(MSVC)
    target_compile_options(sidportal_core PRIVATE /W4)
else()
    target_compile_options(sidportal_core PRIVATE -Wall -Wextra)
endif()

# The realtime desktop host is Linux-specific (ALSA). The JUCE plugin and
# standalone targets use sidportal_core directly and build on macOS without it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SIDPORTAL_CLI_DEFAULT ON)
else()
    set(SIDPORTAL_CLI_DEFAULT OFF)
endif()
option(SIDPORTAL_CLI "Build the Linux ALSA command-line host and panel engine" ${SIDPORTAL_CLI_DEFAULT})
if(SIDPORTAL_CLI)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "SIDPORTAL_CLI requires Linux and ALSA")
    endif()
    find_library(ALSA_LIBRARY asound REQUIRED)
    add_executable(sidportal src/main.cpp)
    target_link_libraries(sidportal PRIVATE sidportal_core ${ALSA_LIBRARY} Threads::Threads)
    target_compile_options(sidportal PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(noise_gate_test tests/noise_gate_test.cpp)
target_include_directories(noise_gate_test PRIVATE src/core)
add_test(NAME noise_gate COMMAND noise_gate_test)
add_executable(hc11_test tests/hc11_test.cpp)
target_link_libraries(hc11_test PRIVATE sidportal_core)
add_test(NAME hc11_core COMMAND hc11_test)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND SIDPORTAL_CLI)
    add_test(NAME noise_gate_render COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_noise_gate_render.py)
    set_tests_properties(noise_gate_render PROPERTIES
        ENVIRONMENT "SIDPORTAL_BINARY=$<TARGET_FILE:sidportal>" SKIP_RETURN_CODE 77)
    add_test(NAME firmware COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_firmware.py)
    set_tests_properties(firmware PROPERTIES
        ENVIRONMENT "SIDPORTAL_BINARY=$<TARGET_FILE:sidportal>;SIDPORTAL_ROOT=${CMAKE_SOURCE_DIR}"
        SKIP_RETURN_CODE 77)
endif()

# VST3 plugin (and a JUCE standalone of it), built when JUCE 8 is found. JUCE is used
# under its AGPLv3 licence; see THIRD_PARTY.md.
set(JUCE_DIR "${CMAKE_SOURCE_DIR}/../JUCE" CACHE PATH "JUCE 8 source directory (default: a checkout beside this one)")
option(SIDPORTAL_PLUGIN "Build the VST3 plugin (needs JUCE_DIR)" ON)
if(SIDPORTAL_PLUGIN AND NOT EXISTS "${JUCE_DIR}/CMakeLists.txt" AND APPLE)
    message(FATAL_ERROR "JUCE 8 is required for the macOS plugin build. Set JUCE_DIR to a JUCE 8 checkout.")
endif()
if(SIDPORTAL_PLUGIN AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

    # The panel artwork, bundled in tools/assets/panel (2x images, layout, LCD font).
    set(ASSET_DIR "${CMAKE_SOURCE_DIR}/tools/assets/panel")
    set(ASSET_FILES 2x/panel.png 2x/knob.png 2x/small_knob.png 2x/encoder.png 2x/slider.png 2x/gear_up.png
        2x/gear_down.png 2x/character.png 2x/led_off.png 2x/led_on.png 2x/dimple.png 2x/subtitle_6581.png
        2x/subtitle_8580.png layout.json)
    foreach(code RANGE 15)
        list(APPEND ASSET_FILES 2x/key${code}_up.png 2x/key${code}_down.png)
    endforeach()
    list(TRANSFORM ASSET_FILES PREPEND "${ASSET_DIR}/" OUTPUT_VARIABLE ASSET_PATHS)
    juce_add_binary_data(SidPortalAssets NAMESPACE SidPortalAssets HEADER_NAME SidPortalAssets.h
        SOURCES ${ASSET_PATHS})
    set_target_properties(SidPortalAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    juce_add_plugin(SidPortalPlugin
        PRODUCT_NAME "SidPortal" COMPANY_NAME "SidPortal" VERSION 0.1.0
        BUNDLE_ID "org.sidportal.sidportal" PLUGIN_MANUFACTURER_CODE Sdse PLUGIN_CODE Sds1
        FORMATS VST3 Standalone IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT TRUE
        VST3_CATEGORIES Instrument Synth
        ICON_BIG "${CMAKE_SOURCE_DIR}/tools/assets/icons/sidportal-128.png"
        ICON_SMALL "${CMAKE_SOURCE_DIR}/tools/assets/icons/sidportal-32.png"
        COPY_PLUGIN_AFTER_BUILD FALSE EDITOR_WANTS_KEYBOARD_FOCUS FALSE)
    target_sources(SidPortalPlugin PRIVATE plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
    target_include_directories(SidPortalPlugin PRIVATE plugin "${ASSET_DIR}")
    target_compile_definitions(SidPortalPlugin PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        SIDPORTAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(SidPortalPlugin PRIVATE sidportal_core SidPortalAssets juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)

    juce_add_console_app(sidportal_plugin_smoke PRODUCT_NAME "sidportal_plugin_smoke" VERSION 0.1.0)
    target_sources(sidportal_plugin_smoke PRIVATE plugin/Smoke.cpp)
    target_include_directories(sidportal_plugin_smoke PRIVATE plugin "${JUCE_DIR}/modules")
    target_compile_definitions(sidportal_plugin_smoke PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(sidportal_plugin_smoke PRIVATE SidPortalPlugin sidportal_core)
    # A minimal VST3 host: loads the built bundle the way a DAW does.
    juce_add_console_app(sidportal_vst3_hostcheck PRODUCT_NAME "sidportal_vst3_hostcheck" VERSION 0.1.0)
    target_sources(sidportal_vst3_hostcheck PRIVATE plugin/HostCheck.cpp)
    target_compile_definitions(sidportal_vst3_hostcheck PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(sidportal_vst3_hostcheck PRIVATE juce::juce_audio_processors juce::juce_audio_utils
        juce::juce_recommended_config_flags)
    add_dependencies(sidportal_vst3_hostcheck SidPortalPlugin_VST3)
    if(EXISTS "${CMAKE_SOURCE_DIR}/roms/firmware.bin")
        add_test(NAME plugin COMMAND sidportal_plugin_smoke)
        add_test(NAME plugin_vst3_host COMMAND sidportal_vst3_hostcheck
            "${CMAKE_BINARY_DIR}/SidPortalPlugin_artefacts/$<CONFIG>/VST3/SidPortal.vst3")
    else()
        # Loading and processing one silent block verifies the bundle and host path
        # without requiring the separately supplied SidStation firmware.
        add_test(NAME plugin_vst3_load COMMAND sidportal_vst3_hostcheck
            "${CMAKE_BINARY_DIR}/SidPortalPlugin_artefacts/$<CONFIG>/VST3/SidPortal.vst3" --load-only)
    endif()
endif()
