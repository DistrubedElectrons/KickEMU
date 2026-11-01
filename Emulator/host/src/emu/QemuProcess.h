#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace kickemu
{
    /**
        Runs QEMU as a child process and streams its stdout/stderr line by line.
        Output and exit callbacks arrive on the reader thread.
    */
    class QemuProcess : private juce::Thread
    {
    public:
        QemuProcess();
        ~QemuProcess() override;

        bool start (const juce::StringArray& args, juce::String& error);

        /** Kills the process if it is still running (use QMP quit first for a clean exit). */
        void kill();

        /** Waits for the process to exit. Returns true if it did within the timeout. */
        bool waitForExit (int timeoutMs);

        bool isRunning() const { return running.load(); }

        std::function<void (const juce::String& line)> onOutput;
        std::function<void (int exitCode)> onExit;

    private:
        void run() override;

        std::unique_ptr<juce::ChildProcess> process;
        std::atomic<bool> running { false };
        juce::WaitableEvent exited;
    };
}
