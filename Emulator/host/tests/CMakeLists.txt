# Unit tests (JUCE UnitTest). Run with ctest or directly:
#   Emulator/out/host/tests/KickEmuTests [category]

add_executable(KickEmuTests
    TestMain.cpp
    ConfigTests.cpp
    LinuxSideParser.cpp
    McuProtocolTests.cpp
    PanelStateTests.cpp
    LayoutTests.cpp
    WindowFitTests.cpp
    QemuCommandLineTests.cpp
    MidiStreamParserTests.cpp
    CvBridgeTests.cpp
    TerminalModelTests.cpp
    SocketTests.cpp
    AutomationTests.cpp)

target_link_libraries(KickEmuTests PRIVATE kickemu_core juce::juce_recommended_warning_flags)
target_compile_definitions(KickEmuTests PRIVATE
    KICKEMU_TEST_REPO_ROOT="${KICKEMU_REPO_ROOT}"
    KICKEMU_TEST_LAYOUT="${CMAKE_CURRENT_SOURCE_DIR}/../resources/layout.json")

add_test(NAME KickEmuTests COMMAND KickEmuTests)
