#include "emu/UnixSocket.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace kickemu::unixsocket
{
    namespace
    {
        bool makeAddress (const juce::String& path, sockaddr_un& addr)
        {
            std::memset (&addr, 0, sizeof (addr));
            addr.sun_family = AF_UNIX;
            auto utf8 = path.toRawUTF8();
            if (std::strlen (utf8) >= sizeof (addr.sun_path))
                return false;
            std::strncpy (addr.sun_path, utf8, sizeof (addr.sun_path) - 1);
            return true;
        }

        void noSigPipe (int fd)
        {
           #ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
           #else
            juce::ignoreUnused (fd);
           #endif
        }
    }

    bool pathFits (const juce::String& path)
    {
        sockaddr_un addr;
        return makeAddress (path, addr);
    }

    int listenAt (const juce::String& path, juce::String& error)
    {
        sockaddr_un addr;
        if (! makeAddress (path, addr))
        {
            error = "socket path too long (" + juce::String (path.getNumBytesAsUTF8()) + " bytes): " + path;
            return -1;
        }

        ::unlink (addr.sun_path);
        int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            error = juce::String ("socket(): ") + juce::String (std::strerror (errno));
            return -1;
        }
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);

        if (::bind (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0
            || ::listen (fd, 2) != 0)
        {
            error = "bind/listen " + path + ": " + juce::String (std::strerror (errno));
            ::close (fd);
            return -1;
        }
        return fd;
    }

    int connectTo (const juce::String& path, juce::String& error)
    {
        sockaddr_un addr;
        if (! makeAddress (path, addr))
        {
            error = "socket path too long: " + path;
            return -1;
        }

        int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            error = juce::String ("socket(): ") + juce::String (std::strerror (errno));
            return -1;
        }
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);
        noSigPipe (fd);

        if (::connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0)
        {
            error = "connect " + path + ": " + juce::String (std::strerror (errno));
            ::close (fd);
            return -1;
        }
        return fd;
    }

    int waitReadable (int fd, int timeoutMs)
    {
        pollfd p { fd, POLLIN, 0 };
        int r;
        do
        {
            r = ::poll (&p, 1, timeoutMs);
        } while (r < 0 && errno == EINTR);

        if (r > 0 && (p.revents & (POLLERR | POLLNVAL)) != 0 && (p.revents & POLLIN) == 0)
            return -1;
        return r;
    }

    int acceptClient (int listenFd)
    {
        int fd = ::accept (listenFd, nullptr, nullptr);
        if (fd >= 0)
        {
            ::fcntl (fd, F_SETFD, FD_CLOEXEC);
            noSigPipe (fd);
        }
        return fd;
    }

    int readSome (int fd, void* buffer, int size)
    {
        ssize_t n;
        do
        {
            n = ::read (fd, buffer, (size_t) size);
        } while (n < 0 && errno == EINTR);
        return (int) n;
    }

    bool writeAll (int fd, const void* data, size_t size)
    {
        auto* p = static_cast<const char*> (data);
        while (size > 0)
        {
            auto n = ::send (fd, p, size, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN)
                {
                    pollfd pf { fd, POLLOUT, 0 };
                    ::poll (&pf, 1, 100);
                    continue;
                }
                return false;
            }
            p += n;
            size -= (size_t) n;
        }
        return true;
    }

    void removeSocketFile (const juce::String& path)
    {
        ::unlink (path.toRawUTF8());
    }

    void closeFd (int& fd)
    {
        if (fd >= 0)
        {
            ::shutdown (fd, SHUT_RDWR);
            ::close (fd);
        }
        fd = -1;
    }
}
