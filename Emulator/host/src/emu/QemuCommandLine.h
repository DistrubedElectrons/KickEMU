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

        /** Host facts that pick "auto" values; injectable for tests. */
        struct Host
        {
            bool isMac = false, isLinux = false, arm64 = false;   // not "linux": GCC predefines it as a macro
            bool kvmUsable = false;          // /dev/kvm can be opened read-write
            bool hvfUsable = false;          // macOS: kern.hv_support (absent in most CI VMs)
            juce::String audioDriver;        // what audiodev=auto resolves to
            int tapFd = -1;                  // macvtap: /dev/tapN opened for QEMU to inherit
            static Host detect();
        };

        /** qemu.accel=auto: hvf on macOS with Hypervisor.framework support, kvm on an arm64 Linux host with /dev/kvm, else tcg. */
        static juce::String resolveAccel (const juce::String& configured, const Host& host);

        static QemuCommandLine build (const Config& config, const Host& host);
    };
}
