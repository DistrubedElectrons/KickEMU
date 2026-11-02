#pragma once

#include <juce_core/juce_core.h>

namespace kickemu
{
    /** Parsed command line of the KickEmu executable. */
    struct CommandLine
    {
        juce::File configFile;          // --config
        juce::String profile;           // --profile
        juce::StringArray overrides;    // --set key=value (repeatable)
        bool headless = false;          // --headless
        bool help = false;              // --help / -h
        bool version = false;           // --version
        bool printCommand = false;      // --print-command
        bool noAutostart = false;       // --no-start
        juce::File screenshot;          // --screenshot <png>: save the window and quit
        juce::File render;              // --render <png>: draw the UI offscreen (no window) and quit
        int screenshotDelayMs = 4000;   // --screenshot-delay <ms>
        juce::String screenshotView;    // --view top|rear|front
        bool presentation = false;      // --presentation
        bool mcp = false;               // --mcp: MCP server on stdio (implies --headless)
        juce::String runDir;            // --run-dir: sockets and state (= --set run_dir=...)
        juce::String expect;            // --expect <regex>: exit 0 when the console prints it
        int timeoutSeconds = 0;         // --timeout <s>: exit 3 when --expect did not match in time
        juce::String error;

        static CommandLine parse (const juce::StringArray& args);
        static juce::String helpText();
        static juce::String versionText();
    };
}
