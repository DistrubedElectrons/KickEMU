#pragma once

#include "config/Config.h"

namespace kickemu
{
    /**
        Builds the QEMU command line from the configuration, following the socket
        table in Emulator/docs/ARCHITECTURE.md. Nothing here is specific to one
        machine: the kick board and the stock virt machine differ only in config.
    */
    struct QemuCommandLine
    {
        juce::StringArray args;        // args[0] is the binary
        juce::StringArray warnings;    // non-fatal problems (missing optional images, ...)
        juce::String error;            // fatal problem; args are not usable

        bool ok() const { return error.isEmpty(); }

        /** Shell-quoted single line, for the log and --print-command. */
        juce::String toShellString() const;

        static QemuCommandLine build (const Config& config);
    };
}
