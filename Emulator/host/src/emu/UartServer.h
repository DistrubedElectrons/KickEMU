#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <mutex>

namespace kickemu
{
    /**
        Listening unix socket for one emulated UART (or the CV chardev).

        The host creates the socket before QEMU starts; QEMU connects as a client
        with reconnect, so either side can restart. One client at a time: a new
        connection replaces the old one. Received bytes are delivered on the
        socket thread; send() may be called from any thread.
    */
    class UartServer : private juce::Thread
    {
    public:
        explicit UartServer (const juce::String& name);
        ~UartServer() override;

        /** Called on the socket thread with each chunk of received bytes. */
        std::function<void (const juce::uint8*, size_t)> onData;

        /** Called on the socket thread when a client connects or disconnects. */
        std::function<void (bool connected)> onConnectionChanged;

        /** Called on the socket thread roughly every tickIntervalMs (timers). */
        std::function<void()> onTick;
        int tickIntervalMs = 20;

        /** Starts listening at path. Returns false and sets error on failure. */
        bool start (const juce::File& socketPath, juce::String& error);
        void stop();

        /** Sends bytes to the connected client. Returns false if nobody is connected. */
        bool send (const void* data, size_t size);

        bool isConnected() const { return clientFd.load() >= 0; }
        juce::File getPath() const { return path; }

        std::atomic<juce::uint64> bytesIn { 0 }, bytesOut { 0 };

    private:
        void run() override;
        void dropClient();

        juce::File path;
        int listenFd = -1;
        std::atomic<int> clientFd { -1 };
        std::mutex writeLock;
    };
}
