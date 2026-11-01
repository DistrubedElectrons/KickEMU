#pragma once

#include <juce_core/juce_core.h>

namespace kickemu
{
    /**
        Minimal POSIX unix-domain stream socket helpers. JUCE's socket classes are
        TCP only, and QEMU talks to us over unix sockets in the run directory.
        All functions return -1 / false on failure and never throw.
    */
    namespace unixsocket
    {
        /** Creates a listening socket at path, removing a stale socket file first. */
        int listenAt (const juce::String& path, juce::String& error);

        /** Connects to a listening socket. Non-blocking connect is not needed here. */
        int connectTo (const juce::String& path, juce::String& error);

        /** Waits up to timeoutMs for fd to become readable. Returns >0 readable,
            0 timeout, <0 error. */
        int waitReadable (int fd, int timeoutMs);

        /** Accepts a pending client (call after waitReadable on a listening fd). */
        int acceptClient (int listenFd);

        /** Reads what is available (at most size bytes). Returns 0 on EOF, <0 on error. */
        int readSome (int fd, void* buffer, int size);

        /** Writes the whole buffer, retrying on partial writes. */
        bool writeAll (int fd, const void* data, size_t size);

        void closeFd (int& fd);

        /** Removes a socket file (juce::File::deleteFile refuses sockets on macOS). */
        void removeSocketFile (const juce::String& path);

        /** True if path fits into sockaddr_un (104 bytes on macOS). */
        bool pathFits (const juce::String& path);
    }
}
