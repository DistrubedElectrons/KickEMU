#include "emu/UartServer.h"
#include "emu/UnixSocket.h"

#include <poll.h>

namespace kickemu
{
    UartServer::UartServer (const juce::String& name) : juce::Thread ("uart " + name) {}

    UartServer::~UartServer() { stop(); }

    bool UartServer::start (const juce::File& socketPath, juce::String& error)
    {
        stop();
        path = socketPath;
        path.getParentDirectory().createDirectory();

        listenFd = unixsocket::listenAt (path.getFullPathName(), error);
        if (listenFd < 0)
            return false;

        startThread (juce::Thread::Priority::high);
        return true;
    }

    void UartServer::stop()
    {
        signalThreadShouldExit();
        stopThread (2000);
        dropClient();
        unixsocket::closeFd (listenFd);
        if (path != juce::File())
            unixsocket::removeSocketFile (path.getFullPathName());
    }

    bool UartServer::send (const void* data, size_t size)
    {
        std::lock_guard<std::mutex> lock (writeLock);
        int fd = clientFd.load();
        if (fd < 0)
            return false;
        if (! unixsocket::writeAll (fd, data, size))
            return false;
        bytesOut += size;
        return true;
    }

    void UartServer::dropClient()
    {
        bool hadClient;
        {
            std::lock_guard<std::mutex> lock (writeLock);
            int fd = clientFd.exchange (-1);
            hadClient = fd >= 0;
            unixsocket::closeFd (fd);
        }
        if (hadClient && onConnectionChanged)
            onConnectionChanged (false);
    }

    void UartServer::run()
    {
        juce::HeapBlock<juce::uint8> buffer (4096);
        auto lastTick = juce::Time::getMillisecondCounter();

        while (! threadShouldExit())
        {
            pollfd fds[2];
            int count = 0;
            fds[count++] = { listenFd, POLLIN, 0 };
            int client = clientFd.load();
            if (client >= 0)
                fds[count++] = { client, POLLIN, 0 };

            int r = ::poll (fds, (nfds_t) count, tickIntervalMs);

            if (r > 0 && (fds[0].revents & POLLIN) != 0)
            {
                int fd = unixsocket::acceptClient (listenFd);
                if (fd >= 0)
                {
                    dropClient();
                    clientFd = fd;
                    if (onConnectionChanged)
                        onConnectionChanged (true);
                }
            }

            if (r > 0 && count > 1 && fds[1].revents != 0)
            {
                int n = unixsocket::readSome (client, buffer, 4096);
                if (n > 0)
                {
                    bytesIn += (juce::uint64) n;
                    if (onData)
                        onData (buffer, (size_t) n);
                }
                else
                {
                    dropClient();
                }
            }

            auto now = juce::Time::getMillisecondCounter();
            if (onTick && now - lastTick >= (juce::uint32) tickIntervalMs)
            {
                lastTick = now;
                onTick();
            }
        }
    }
}
