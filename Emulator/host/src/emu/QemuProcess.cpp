#include "emu/QemuProcess.h"

namespace kickemu
{
    QemuProcess::QemuProcess() : juce::Thread ("qemu output") {}

    QemuProcess::~QemuProcess()
    {
        kill();
        stopThread (2000);
    }

    bool QemuProcess::start (const juce::StringArray& args, juce::String& error)
    {
        if (running)
        {
            error = "QEMU is already running";
            return false;
        }
        stopThread (1000);

        process = std::make_unique<juce::ChildProcess>();
        if (! process->start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            error = "Could not start " + args[0];
            process.reset();
            return false;
        }

        exited.reset();
        running = true;
        startThread();
        return true;
    }

    void QemuProcess::kill()
    {
        if (process != nullptr && running)
            process->kill();
    }

    bool QemuProcess::waitForExit (int timeoutMs)
    {
        if (! running)
            return true;
        return exited.wait (timeoutMs);
    }

    void QemuProcess::run()
    {
        juce::MemoryBlock pending;
        char buffer[4096];

        auto flushLines = [this, &pending] (bool all)
        {
            auto text = juce::String::fromUTF8 (static_cast<const char*> (pending.getData()), (int) pending.getSize());
            int consumed = 0;
            for (;;)
            {
                int nl = text.indexOfChar (consumed, '\n');
                if (nl < 0)
                    break;
                if (onOutput)
                    onOutput (text.substring (consumed, nl).trimCharactersAtEnd ("\r"));
                consumed = nl + 1;
            }
            if (all && consumed < text.length())
            {
                if (onOutput)
                    onOutput (text.substring (consumed));
                consumed = text.length();
            }
            auto rest = text.substring (consumed);
            pending.reset();
            pending.append (rest.toRawUTF8(), rest.getNumBytesAsUTF8());
        };

        while (! threadShouldExit())
        {
            int n = process->readProcessOutput (buffer, sizeof (buffer));
            if (n <= 0)
                break;
            pending.append (buffer, (size_t) n);
            flushLines (false);
        }
        flushLines (true);

        // Reap the child and report its exit code.
        while (process->isRunning() && ! threadShouldExit())
            process->waitForProcessToFinish (100);

        const auto code = (int) process->getExitCode();
        running = false;
        exited.signal();
        if (onExit)
            onExit (code);
    }
}
