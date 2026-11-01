# KickEmu host application: front panel, launcher, MCU emulator and I/O bridges.
#
# Build (from the repository root):
#   cmake -S Emulator/host -B Emulator/out/host -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build Emulator/out/host
#   ctest --test-dir Emulator/out/host --output-on-failure

cmake_minimum_required(VERSION 3.22)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" KICKEMU_VERSION LIMIT_COUNT 1)
project(KickEmu VERSION ${KICKEMU_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(KICKEMU_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(KICKEMU_JUCE_DIR "${KICKEMU_REPO_ROOT}/Emulator/third_party/JUCE" CACHE PATH "JUCE source tree")
set(KICKEMU_PKTMGR_DIR "${KICKEMU_REPO_ROOT}/ComProtocol/PktMgr" CACHE PATH "PktMgr framing sources")

if(NOT EXISTS "${KICKEMU_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found. Run: git submodule update --init --depth 1 Emulator/third_party/JUCE")
endif()

add_subdirectory("${KICKEMU_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ------------------------------------------------------------------------------
# Build information (version, branch, commit), regenerated on every build so
# the about box and --help always show the commit the binary was built from.
set(KICKEMU_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(kickemu_buildinfo
    COMMAND ${CMAKE_COMMAND}
        -DREPO_ROOT=${KICKEMU_REPO_ROOT}
        -DVERSION=${PROJECT_VERSION}
        -DOUTPUT=${KICKEMU_GENERATED_DIR}/BuildInfo.h
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake"
    BYPRODUCTS "${KICKEMU_GENERATED_DIR}/BuildInfo.h"
    COMMENT "Generating build information"
    VERBATIM)

# ------------------------------------------------------------------------------
# JUCE modules compiled once into a static library shared by the app and the
# tests (JUCE modules are interface libraries that add their sources to every
# consumer, so they are linked privately here and re-exported as flags).
add_library(kickemu_juce STATIC)
target_link_libraries(kickemu_juce
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_data_structures
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_basics
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
target_compile_definitions(kickemu_juce
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    INTERFACE
        $<TARGET_PROPERTY:kickemu_juce,COMPILE_DEFINITIONS>)
target_include_directories(kickemu_juce SYSTEM INTERFACE
    $<TARGET_PROPERTY:kickemu_juce,INCLUDE_DIRECTORIES>)
set_target_properties(kickemu_juce PROPERTIES
    POSITION_INDEPENDENT_CODE TRUE
    VISIBILITY_INLINES_HIDDEN TRUE
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)

# ------------------------------------------------------------------------------
# Core library: everything that does not need a window (config, protocol,
# sockets, QEMU launcher, RFB/QMP clients, bridges). Shared by the app and tests.
add_library(kickemu_core STATIC
    src/config/Config.cpp
    src/config/Layout.cpp
    src/config/WindowFit.cpp
    src/emu/UnixSocket.cpp
    src/emu/UartServer.cpp
    src/emu/PanelState.cpp
    src/emu/McuProtocol.cpp
    src/emu/McuEmulator.cpp
    src/emu/QemuCommandLine.cpp
    src/emu/QemuProcess.cpp
    src/emu/QmpClient.cpp
    src/emu/RfbClient.cpp
    src/emu/MidiStreamParser.cpp
    src/emu/CvBridge.cpp
    src/emu/TerminalModel.cpp
    src/emu/MidiBridge.cpp
    src/emu/ConsoleBridge.cpp
    "${KICKEMU_PKTMGR_DIR}/PktMgr.cpp")
add_dependencies(kickemu_core kickemu_buildinfo)
target_include_directories(kickemu_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${KICKEMU_GENERATED_DIR}")
# Firmware sources shared with the device: included as system headers so their
# own conventions do not trip our warning flags.
target_include_directories(kickemu_core SYSTEM PUBLIC
    "${KICKEMU_PKTMGR_DIR}/.."
    "${KICKEMU_REPO_ROOT}/Kick/modules")
target_link_libraries(kickemu_core
    PUBLIC kickemu_juce
    PRIVATE juce::juce_recommended_warning_flags)
# PktMgr is shared with the device firmware and keeps its own conventions.
set_source_files_properties("${KICKEMU_PKTMGR_DIR}/PktMgr.cpp" PROPERTIES COMPILE_OPTIONS "-w")

option(KICKEMU_BUILD_APP "Build the KickEmu GUI application (off: core library and tests only)" ON)

# ------------------------------------------------------------------------------
# Resources (layout geometry), embedded so the app runs from any directory.
if(KICKEMU_BUILD_APP)
juce_add_binary_data(kickemu_resources
    HEADER_NAME KickEmuResources.h
    NAMESPACE KickEmuResources
    SOURCES resources/layout.json)

# ------------------------------------------------------------------------------
# The application.
juce_add_gui_app(KickEmu
    PRODUCT_NAME "KickEmu"
    COMPANY_NAME "KickEmu"
    BUNDLE_ID "org.kickemu.host"
    VERSION ${PROJECT_VERSION})

target_sources(KickEmu PRIVATE
    src/app/Main.cpp
    src/app/AppSettings.cpp
    src/app/CommandLine.cpp
    src/app/Session.cpp
    src/app/MainComponent.cpp
    src/app/TitleBar.cpp
    src/app/SidePanel.cpp
    src/app/LogView.cpp
    src/app/TerminalView.cpp
    src/app/SettingsView.cpp
    src/panel/Theme.cpp
    src/panel/Painting.cpp
    src/panel/DeviceView.cpp
    src/panel/TopPanel.cpp
    src/panel/PadComponent.cpp
    src/panel/ButtonComponent.cpp
    src/panel/KnobComponent.cpp
    src/panel/JogWheelComponent.cpp
    src/panel/SliderComponent.cpp
    src/panel/ScreenComponent.cpp
    src/panel/RearPanel.cpp
    src/panel/PanelControl.cpp)

target_compile_definitions(KickEmu PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:KickEmu,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:KickEmu,JUCE_VERSION>")

target_link_libraries(KickEmu PRIVATE
    kickemu_core
    kickemu_resources
    juce::juce_recommended_warning_flags)
endif()

# ------------------------------------------------------------------------------
# Tests.
enable_testing()
add_subdirectory(tests)
