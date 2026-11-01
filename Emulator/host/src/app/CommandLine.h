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
        int screenshotDelayMs = 4000;   // --screenshot-delay <ms>
        juce::String screenshotView;    // --view top|rear|front
        bool presentation = false;      // --presentation
        juce::String error;

        static CommandLine parse (const juce::StringArray& args);
        static juce::String helpText();
        static juce::String versionText();
    };
}
