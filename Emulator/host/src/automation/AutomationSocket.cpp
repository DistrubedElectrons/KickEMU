#include "automation/AutomationSocket.h"

#include "emu/UnixSocket.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kickemu
{
    AutomationSocket::AutomationSocket (McpServer& s) : server (s) {}

    AutomationSocket::~AutomationSocket() { stop(); }

    bool AutomationSocket::start (const juce::File& socketPath, juce::String& error)
    {
        stop();
        path = socketPath;
        listenFd = unixsocket::listenAt (path.getFullPathName(), error);
        if (listenFd < 0)
            return false;
        if (::pipe (wakePipe) != 0)
        {
            error = "pipe() failed";
            unixsocket::closeFd (listenFd);
            return false;
        }
        stopping = false;
        acceptThread = std::thread ([this] { acceptLoop(); });
        return true;
    }

    void AutomationSocket::stop()
    {
        if (! acceptThread.joinable())
            return;
        stopping = true;
        const char b = 1;
        [[maybe_unused]] auto n = ::write (wakePipe[1], &b, 1);
        acceptThread.join();

        {
            std::lock_guard<std::mutex> guard (clientsLock);
            // Unblock client reads; each thread then exits and is joined here.
            for (auto& c : clients)
                if (c.fd >= 0)
                    ::shutdown (c.fd, SHUT_RDWR);
        }
        for (auto& c : clients)
            if (c.thread.joinable())
                c.thread.join();
        for (auto& c : clients)
            unixsocket::closeFd (c.fd);
        clients.clear();

        unixsocket::closeFd (listenFd);
        unixsocket::closeFd (wakePipe[0]);
        unixsocket::closeFd (wakePipe[1]);
        unixsocket::removeSocketFile (path.getFullPathName());
    }

    void AutomationSocket::reapFinished()
    {
        std::lock_guard<std::mutex> guard (clientsLock);
        for (auto it = clients.begin(); it != clients.end();)
        {
            if (it->finished.load())
            {
                it->thread.join();
                unixsocket::closeFd (it->fd);
                it = clients.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    void AutomationSocket::acceptLoop()
    {
        while (! stopping.load())
        {
            pollfd fds[2] { { listenFd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
            if (::poll (fds, 2, 500) < 0)
                continue;
            reapFinished();
            if (stopping.load() || (fds[1].revents & POLLIN) != 0)
                break;
            if ((fds[0].revents & POLLIN) == 0)
                continue;

            int fd = unixsocket::acceptClient (listenFd);
            if (fd < 0)
                continue;
            std::lock_guard<std::mutex> guard (clientsLock);
            if ((int) clients.size() >= maxClients)
            {
                static const char busy[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,\"message\":\"too many clients\"}}\n";
                unixsocket::writeAll (fd, busy, sizeof (busy) - 1);
                unixsocket::closeFd (fd);
                continue;
            }
            auto& c = clients.emplace_back();
            c.fd = fd;
            c.thread = std::thread ([this, &c] { serveClient (c); });
        }
    }

    void AutomationSocket::serveClient (Client& client)
    {
        std::string line;
        char buffer[4096];
        for (;;)
        {
            int n = unixsocket::readSome (client.fd, buffer, sizeof (buffer));
            if (n <= 0)
                break;
            for (int i = 0; i < n; ++i)
            {
                if (buffer[i] != '\n')
                {
                    // Bound a single request; nothing legitimate comes close.
                    if (line.size() < 16 * 1024 * 1024)
                        line.push_back (buffer[i]);
                    continue;
                }
                auto text = juce::String::fromUTF8 (line.data(), (int) line.size()).trim();
                line.clear();
                if (text.isEmpty())
                    continue;
                if (auto response = server.handle (text))
                {
                    auto out = *response + "\n";
                    if (! unixsocket::writeAll (client.fd, out.toRawUTF8(), out.getNumBytesAsUTF8()))
                    {
                        client.finished = true;
                        return;
                    }
                }
            }
        }
        client.finished = true;
    }
}
