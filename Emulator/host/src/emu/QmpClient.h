#pragma once

#include <juce_core/juce_core.h>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

namespace kickemu
{
    /**
        QMP client on qmp.sock (QEMU is the server).

        Connects with retry, negotiates capabilities, then sends queued commands
        with ids and routes replies to their callbacks. Asynchronous events go to
        onEvent. All callbacks run on the client thread.
    */
    class QmpClient : private juce::Thread
    {
    public:
        using Callback = std::function<void (const juce::var& result, const juce::String& error)>;

        QmpClient();
        ~QmpClient() override;

        void start (const juce::File& socketPath, int retryMs);
        void stop();

        /** Queues a command; it is sent once the session is ready. */
        void execute (const juce::String& command, const juce::var& arguments = {}, Callback callback = {});

        bool isReady() const { return ready.load(); }

        std::function<void (const juce::String& event, const juce::var& data)> onEvent;
        std::function<void (bool ready)> onReadyChanged;
        std::function<void (const juce::String&)> onLog;

    private:
        struct Pending
        {
            juce::String json;
            int id;
            Callback callback;
        };

        void run() override;
        bool session (int fd);
        void handleMessage (const juce::var& msg, int fd);
        bool flushQueue (int fd);
        void failAll (const juce::String& reason);
        void poke();

        juce::File path;
        int retryIntervalMs = 300;
        std::atomic<bool> ready { false };
        std::atomic<int> currentFd { -1 };

        std::mutex lock;
        std::deque<Pending> queue;
        std::map<int, Callback> inFlight;
        int nextId = 1;
        juce::WaitableEvent wake;
        int wakePipe[2] { -1, -1 };
    };
}
