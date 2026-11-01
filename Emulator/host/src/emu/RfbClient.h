#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <functional>
#include <mutex>

namespace kickemu
{
    /**
        The guest framebuffer shared between the RFB thread (writer) and the
        screen component (reader). The writer updates only the rectangles the
        server sent, under the lock; `serial` tells the UI that something changed.
    */
    struct SharedFramebuffer
    {
        std::mutex lock;
        juce::Image image;                       // ARGB, opaque, native image type
        juce::Rectangle<int> dirty;              // area changed since the UI last looked
        std::atomic<juce::uint32> serial { 0 };
        std::atomic<bool> connected { false };
        juce::String desktopName;
    };

    /**
        Minimal RFB 3.8 client for QEMU's VNC server on a unix socket.

        Security type None, 32 bpp true colour in the native little-endian ARGB
        layout (so raw rectangles are a straight copy), encodings Raw, CopyRect and
        the DesktopSize pseudo-encoding. After every update an incremental request
        is sent, so QEMU pushes changes as they happen.
    */
    class RfbClient : private juce::Thread
    {
    public:
        explicit RfbClient (SharedFramebuffer& target);
        ~RfbClient() override;

        void start (const juce::File& socketPath, int retryMs);
        void stop();

        std::function<void (const juce::String&)> onLog;
        std::function<void()> onFrame;            // called after each complete update

        std::atomic<juce::uint64> bytesReceived { 0 }, updates { 0 };

    private:
        void run() override;
        bool session (int fd);
        bool readExact (int fd, void* dest, size_t size);
        bool writeAll (int fd, const void* data, size_t size);
        bool requestUpdate (int fd, bool incremental);
        void resize (int w, int h);

        SharedFramebuffer& fb;
        juce::File path;
        int retryIntervalMs = 300;
        std::atomic<int> currentFd { -1 };
        int width = 0, height = 0;
        juce::HeapBlock<juce::uint8> rowBuffer;
        size_t rowBufferSize = 0;
    };
}
