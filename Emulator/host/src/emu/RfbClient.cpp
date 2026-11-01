#include "emu/RfbClient.h"
#include "emu/UnixSocket.h"

#include <sys/socket.h>

namespace kickemu
{
    namespace
    {
        constexpr juce::int32 encodingRaw = 0;
        constexpr juce::int32 encodingCopyRect = 1;
        constexpr juce::int32 encodingDesktopSize = -223;

        juce::uint16 be16 (const juce::uint8* p) { return (juce::uint16) ((p[0] << 8) | p[1]); }
        juce::uint32 be32 (const juce::uint8* p) { return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | p[3]; }
        void put16 (juce::uint8* p, int v) { p[0] = (juce::uint8) (v >> 8); p[1] = (juce::uint8) v; }
        void put32 (juce::uint8* p, juce::int32 v) { p[0] = (juce::uint8) (v >> 24); p[1] = (juce::uint8) (v >> 16); p[2] = (juce::uint8) (v >> 8); p[3] = (juce::uint8) v; }
    }

    RfbClient::RfbClient (SharedFramebuffer& target) : juce::Thread ("rfb"), fb (target) {}

    RfbClient::~RfbClient() { stop(); }

    void RfbClient::start (const juce::File& socketPath, int retryMs)
    {
        stop();
        path = socketPath;
        retryIntervalMs = juce::jmax (50, retryMs);
        startThread (juce::Thread::Priority::high);
    }

    void RfbClient::stop()
    {
        signalThreadShouldExit();
        if (int fd = currentFd.load(); fd >= 0)
            ::shutdown (fd, SHUT_RDWR);
        stopThread (2000);
    }

    bool RfbClient::readExact (int fd, void* dest, size_t size)
    {
        auto* p = static_cast<juce::uint8*> (dest);
        while (size > 0)
        {
            if (threadShouldExit())
                return false;
            int r = unixsocket::waitReadable (fd, 200);
            if (r < 0)
                return false;
            if (r == 0)
                continue;
            int n = unixsocket::readSome (fd, p, (int) juce::jmin (size, (size_t) 1 << 20));
            if (n <= 0)
                return false;
            p += n;
            size -= (size_t) n;
            bytesReceived += (juce::uint64) n;
        }
        return true;
    }

    bool RfbClient::writeAll (int fd, const void* data, size_t size)
    {
        return unixsocket::writeAll (fd, data, size);
    }

    bool RfbClient::requestUpdate (int fd, bool incremental)
    {
        juce::uint8 msg[10] = { 3, (juce::uint8) (incremental ? 1 : 0) };
        put16 (msg + 2, 0);
        put16 (msg + 4, 0);
        put16 (msg + 6, width);
        put16 (msg + 8, height);
        return writeAll (fd, msg, sizeof (msg));
    }

    void RfbClient::resize (int w, int h)
    {
        width = w;
        height = h;
        std::lock_guard<std::mutex> guard (fb.lock);
        fb.image = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true, juce::NativeImageType());
        fb.image.clear (fb.image.getBounds(), juce::Colours::black);
        fb.dirty = fb.image.getBounds();
        fb.serial++;
    }

    bool RfbClient::session (int fd)
    {
        // --- version -------------------------------------------------------------------
        char version[12];
        if (! readExact (fd, version, 12))
            return false;
        if (std::memcmp (version, "RFB 003.", 8) != 0)
        {
            if (onLog) onLog ("VNC: unexpected greeting");
            return false;
        }
        if (! writeAll (fd, "RFB 003.008\n", 12))
            return false;

        // --- security: None -----------------------------------------------------------
        juce::uint8 count = 0;
        if (! readExact (fd, &count, 1))
            return false;
        if (count == 0)
        {
            juce::uint8 len[4];
            if (readExact (fd, len, 4))
            {
                juce::HeapBlock<char> reason (be32 (len) + 1, true);
                readExact (fd, reason, be32 (len));
                if (onLog) onLog ("VNC refused: " + juce::String (reason.get()));
            }
            return false;
        }
        juce::uint8 types[256];
        if (! readExact (fd, types, count))
            return false;
        bool hasNone = false;
        for (int i = 0; i < count; ++i)
            hasNone |= types[i] == 1;
        if (! hasNone)
        {
            if (onLog) onLog ("VNC: server requires authentication; disable it for the unix socket");
            return false;
        }
        juce::uint8 choice = 1;
        juce::uint8 result[4];
        if (! writeAll (fd, &choice, 1) || ! readExact (fd, result, 4) || be32 (result) != 0)
            return false;

        // --- init ------------------------------------------------------------------------
        juce::uint8 shared = 1;
        juce::uint8 serverInit[24];
        if (! writeAll (fd, &shared, 1) || ! readExact (fd, serverInit, 24))
            return false;
        const int w = be16 (serverInit), h = be16 (serverInit + 2);
        const auto nameLen = be32 (serverInit + 20);
        juce::HeapBlock<char> name (nameLen + 1, true);
        if (! readExact (fd, name, nameLen))
            return false;
        fb.desktopName = juce::String::fromUTF8 (name, (int) nameLen);
        resize (w, h);

        // --- our pixel format: 32 bpp, little endian, X8R8G8B8 = JUCE's PixelARGB --------
        juce::uint8 setPf[20] = { 0, 0, 0, 0, 32, 24, 0, 1 };
        put16 (setPf + 8, 255);
        put16 (setPf + 10, 255);
        put16 (setPf + 12, 255);
        setPf[14] = 16;
        setPf[15] = 8;
        setPf[16] = 0;
        if (! writeAll (fd, setPf, sizeof (setPf)))
            return false;

        const juce::int32 encodings[] = { encodingCopyRect, encodingRaw, encodingDesktopSize };
        juce::uint8 setEnc[4 + 4 * 3] = { 2, 0 };
        put16 (setEnc + 2, 3);
        for (int i = 0; i < 3; ++i)
            put32 (setEnc + 4 + 4 * i, encodings[i]);
        if (! writeAll (fd, setEnc, sizeof (setEnc)) || ! requestUpdate (fd, false))
            return false;

        fb.connected = true;
        if (onLog) onLog ("VNC connected: " + juce::String (w) + "x" + juce::String (h) + " \"" + fb.desktopName + "\"");

        // --- message loop ------------------------------------------------------------------
        while (! threadShouldExit())
        {
            juce::uint8 type;
            if (! readExact (fd, &type, 1))
                return false;

            if (type == 0) // FramebufferUpdate
            {
                juce::uint8 hdr[3];
                if (! readExact (fd, hdr, 3))
                    return false;
                const int rects = be16 (hdr + 1);

                for (int r = 0; r < rects; ++r)
                {
                    juce::uint8 rh[12];
                    if (! readExact (fd, rh, 12))
                        return false;
                    const int x = be16 (rh), y = be16 (rh + 2), rw = be16 (rh + 4), rhgt = be16 (rh + 6);
                    const auto enc = (juce::int32) be32 (rh + 8);

                    if (enc == encodingRaw)
                    {
                        const size_t rowBytes = (size_t) rw * 4;
                        if (rowBufferSize < rowBytes * (size_t) rhgt)
                        {
                            rowBufferSize = rowBytes * (size_t) rhgt;
                            rowBuffer.realloc (rowBufferSize);
                        }
                        // Read the whole rectangle first so the lock is held only for the copy.
                        if (! readExact (fd, rowBuffer, rowBytes * (size_t) rhgt))
                            return false;

                        juce::Rectangle<int> area (x, y, rw, rhgt);
                        std::lock_guard<std::mutex> guard (fb.lock);
                        area = area.getIntersection (fb.image.getBounds());
                        if (! area.isEmpty())
                        {
                            juce::Image::BitmapData bits (fb.image, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                                                          juce::Image::BitmapData::writeOnly);
                            for (int row = 0; row < area.getHeight(); ++row)
                            {
                                auto* src = reinterpret_cast<const juce::uint32*> (rowBuffer.get() + rowBytes * (size_t) (row + area.getY() - y))
                                            + (area.getX() - x);
                                auto* dst = reinterpret_cast<juce::uint32*> (bits.getLinePointer (row));
                                for (int col = 0; col < area.getWidth(); ++col)
                                    dst[col] = src[col] | 0xff000000u;
                            }
                            fb.dirty = fb.dirty.isEmpty() ? area : fb.dirty.getUnion (area);
                        }
                    }
                    else if (enc == encodingCopyRect)
                    {
                        juce::uint8 src[4];
                        if (! readExact (fd, src, 4))
                            return false;
                        std::lock_guard<std::mutex> guard (fb.lock);
                        fb.image.moveImageSection (x, y, be16 (src), be16 (src + 2), rw, rhgt);
                        auto area = juce::Rectangle<int> (x, y, rw, rhgt).getIntersection (fb.image.getBounds());
                        fb.dirty = fb.dirty.isEmpty() ? area : fb.dirty.getUnion (area);
                    }
                    else if (enc == encodingDesktopSize)
                    {
                        resize (rw, rhgt);
                        if (onLog) onLog ("VNC resolution " + juce::String (rw) + "x" + juce::String (rhgt));
                    }
                    else
                    {
                        if (onLog) onLog ("VNC: unsupported encoding " + juce::String (enc));
                        return false;
                    }
                }

                fb.serial++;
                updates++;
                if (onFrame)
                    onFrame();
                if (! requestUpdate (fd, true))
                    return false;
            }
            else if (type == 1) // SetColourMapEntries (not used with true colour, skip)
            {
                juce::uint8 hdr[5];
                if (! readExact (fd, hdr, 5))
                    return false;
                juce::HeapBlock<juce::uint8> skip ((size_t) be16 (hdr + 3) * 6 + 1);
                if (! readExact (fd, skip, (size_t) be16 (hdr + 3) * 6))
                    return false;
            }
            else if (type == 2) // Bell
            {
            }
            else if (type == 3) // ServerCutText
            {
                juce::uint8 hdr[7];
                if (! readExact (fd, hdr, 7))
                    return false;
                const auto len = be32 (hdr + 3);
                juce::HeapBlock<juce::uint8> skip (len + 1);
                if (! readExact (fd, skip, len))
                    return false;
            }
            else
            {
                if (onLog) onLog ("VNC: unknown server message " + juce::String (type));
                return false;
            }
        }
        return true;
    }

    void RfbClient::run()
    {
        bool loggedWaiting = false;
        while (! threadShouldExit())
        {
            juce::String error;
            int fd = unixsocket::connectTo (path.getFullPathName(), error);
            if (fd < 0)
            {
                if (! loggedWaiting && onLog)
                    onLog ("Waiting for VNC at " + path.getFullPathName());
                loggedWaiting = true;
                wait (retryIntervalMs);
                continue;
            }

            loggedWaiting = false;
            currentFd = fd;
            session (fd);
            currentFd = -1;
            unixsocket::closeFd (fd);
            if (fb.connected.exchange (false) && onLog)
                onLog ("VNC disconnected");
            fb.serial++;
            wait (retryIntervalMs);
        }
    }
}
