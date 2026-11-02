#pragma once

#include "emu/ConsoleBridge.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace kickemu
{
    /**
        Client for the QEMU guest agent (qemu-ga) running in the guest on UART2
        (/dev/ttymxc1, `qemu-ga -m isa-serial -p /dev/ttymxc1`). Copies files in and out
        and runs programs with their exit code and output, independent of the network
        and the console.

        Protocol: QMP-style JSON, one object per line. Every operation starts with
        guest-sync-delimited (a 0xFF byte, then the request; the guest answers 0xFF and
        the id), which discards anything left over from an earlier, interrupted call.
        File data travels base64-encoded (guest-file-read/write), so throughput is that
        of the UART path in QEMU, not of the guest's network.

        All calls block and must not run on the message thread. One operation at a time.
    */
    class GuestAgent
    {
    public:
        explicit GuestAgent (ConsoleBridge& uart);
        ~GuestAgent();

        /** Agent version, or an error when no agent answers within @timeoutMs. */
        juce::Result ping (juce::String& version, int timeoutMs = 3000);

        /** Copies @local into the guest as @remote; @mode (e.g. 0755) is applied when >= 0. */
        juce::Result push (const juce::File& local, const juce::String& remote, int mode,
                           std::function<void (juce::int64 done, juce::int64 total)> progress = {});
        /** Copies @remote out of the guest into @local. */
        juce::Result pull (const juce::String& remote, const juce::File& local, juce::int64& bytes);

        struct ExecResult
        {
            int exitCode = -1;
            int signal = 0;
            bool timedOut = false;
            juce::int64 pid = 0;
            juce::MemoryBlock out, err;
        };
        /** Runs @path with @args (no shell); @input goes to its stdin. */
        juce::Result exec (const juce::String& path, const juce::StringArray& args, const juce::MemoryBlock& input,
                           int timeoutMs, ExecResult& result);

        /** Traffic seen from an agent (true once it answered). */
        bool hasAnswered() const { return answered; }

    private:
        juce::var call (const juce::String& command, const juce::var& arguments, int timeoutMs, juce::String& error);
        juce::Result sync (int timeoutMs);
        void received (const juce::uint8* data, size_t size);
        bool waitForLine (juce::var& out, int timeoutMs);

        ConsoleBridge& uart;
        std::mutex opLock;              // one operation at a time

        std::mutex rxLock;
        std::condition_variable rxReady;
        std::string partial;
        std::deque<juce::var> lines;
        std::atomic<bool> answered { false };
        juce::int64 nextSyncId;
    };
}
