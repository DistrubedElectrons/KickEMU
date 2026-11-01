#include "emu/QmpClient.h"
#include "emu/UnixSocket.h"

#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kickemu
{
    QmpClient::QmpClient() : juce::Thread ("qmp")
    {
        // Self-pipe so a queued command wakes the session immediately.
        if (::pipe (wakePipe) != 0)
            wakePipe[0] = wakePipe[1] = -1;
    }

    QmpClient::~QmpClient()
    {
        stop();
        failAll ("QMP client destroyed");
        unixsocket::closeFd (wakePipe[0]);
        unixsocket::closeFd (wakePipe[1]);
    }

    void QmpClient::poke()
    {
        wake.signal();
        if (wakePipe[1] >= 0)
        {
            char c = 1;
            [[maybe_unused]] auto n = ::write (wakePipe[1], &c, 1);
        }
    }

    void QmpClient::start (const juce::File& socketPath, int retryMs)
    {
        stop();
        path = socketPath;
        retryIntervalMs = juce::jmax (50, retryMs);
        startThread();
    }

    void QmpClient::stop()
    {
        signalThreadShouldExit();
        poke();
        if (int fd = currentFd.load(); fd >= 0)
            ::shutdown (fd, SHUT_RDWR);
        stopThread (2000);

        // Commands already sent will never be answered; queued ones wait for the next start().
        std::map<int, Callback> lost;
        {
            std::lock_guard<std::mutex> guard (lock);
            lost.swap (inFlight);
        }
        for (auto& [id, cb] : lost)
            if (cb)
                cb ({}, "QMP client stopped");
    }

    void QmpClient::execute (const juce::String& command, const juce::var& arguments, Callback callback)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("execute", command);
        if (! arguments.isVoid())
            obj->setProperty ("arguments", arguments);

        {
            std::lock_guard<std::mutex> guard (lock);
            int id = nextId++;
            obj->setProperty ("id", id);
            queue.push_back ({ juce::JSON::toString (juce::var (obj), true), id, std::move (callback) });
        }
        poke();
    }

    void QmpClient::failAll (const juce::String& reason)
    {
        std::deque<Pending> dropped;
        std::map<int, Callback> lost;
        {
            std::lock_guard<std::mutex> guard (lock);
            dropped.swap (queue);
            lost.swap (inFlight);
        }
        for (auto& p : dropped)
            if (p.callback)
                p.callback ({}, reason);
        for (auto& [id, cb] : lost)
            if (cb)
                cb ({}, reason);
    }

    bool QmpClient::flushQueue (int fd)
    {
        for (;;)
        {
            Pending p;
            {
                std::lock_guard<std::mutex> guard (lock);
                if (queue.empty())
                    return true;
                p = std::move (queue.front());
                queue.pop_front();
                inFlight[p.id] = std::move (p.callback);
            }
            auto line = p.json + "\n";
            if (! unixsocket::writeAll (fd, line.toRawUTF8(), line.getNumBytesAsUTF8()))
                return false;
        }
    }

    void QmpClient::handleMessage (const juce::var& msg, int fd)
    {
        if (msg.hasProperty ("QMP"))
        {
            // Greeting: enter command mode.
            auto line = juce::String (R"({"execute":"qmp_capabilities","id":0})") + "\n";
            unixsocket::writeAll (fd, line.toRawUTF8(), line.getNumBytesAsUTF8());
            return;
        }

        if (msg.hasProperty ("event"))
        {
            if (onEvent)
                onEvent (msg["event"].toString(), msg["data"]);
            return;
        }

        const int id = msg.hasProperty ("id") ? (int) msg["id"] : -1;
        if (id == 0)
        {
            ready = true;
            if (onLog)
                onLog ("QMP ready");
            if (onReadyChanged)
                onReadyChanged (true);
            return;
        }

        Callback cb;
        {
            std::lock_guard<std::mutex> guard (lock);
            if (auto it = inFlight.find (id); it != inFlight.end())
            {
                cb = std::move (it->second);
                inFlight.erase (it);
            }
        }

        if (msg.hasProperty ("error"))
        {
            auto err = msg["error"]["class"].toString() + ": " + msg["error"]["desc"].toString();
            if (cb)
                cb ({}, err);
            else if (onLog)
                onLog ("QMP error: " + err);
        }
        else if (cb)
        {
            cb (msg["return"], {});
        }
    }

    bool QmpClient::session (int fd)
    {
        juce::MemoryBlock pending;
        char buffer[8192];

        while (! threadShouldExit())
        {
            if (ready && ! flushQueue (fd))
                return false;

            pollfd fds[2] = { { fd, POLLIN, 0 }, { wakePipe[0], POLLIN, 0 } };
            int r = ::poll (fds, wakePipe[0] >= 0 ? 2 : 1, 200);
            if (r < 0 && errno != EINTR)
                return false;
            if (r > 0 && wakePipe[0] >= 0 && (fds[1].revents & POLLIN) != 0)
            {
                char drain[64];
                [[maybe_unused]] auto d = ::read (wakePipe[0], drain, sizeof (drain));
            }
            if (r <= 0 || (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                continue;

            int n = unixsocket::readSome (fd, buffer, sizeof (buffer));
            if (n <= 0)
                return false;
            pending.append (buffer, (size_t) n);

            // QMP messages are one JSON object per line.
            for (;;)
            {
                auto* data = static_cast<const char*> (pending.getData());
                auto size = pending.getSize();
                auto* nl = static_cast<const char*> (std::memchr (data, '\n', size));
                if (nl == nullptr)
                    break;
                auto line = juce::String::fromUTF8 (data, (int) (nl - data)).trim();
                pending.removeSection (0, (size_t) (nl - data) + 1);
                if (line.isEmpty())
                    continue;

                juce::var msg;
                if (juce::JSON::parse (line, msg).wasOk())
                    handleMessage (msg, fd);
            }
        }
        return true;
    }

    void QmpClient::run()
    {
        bool loggedWaiting = false;
        while (! threadShouldExit())
        {
            juce::String error;
            int fd = unixsocket::connectTo (path.getFullPathName(), error);
            if (fd < 0)
            {
                if (! loggedWaiting && onLog)
                    onLog ("Waiting for QMP at " + path.getFullPathName());
                loggedWaiting = true;
                wake.wait (retryIntervalMs);
                continue;
            }

            loggedWaiting = false;
            currentFd = fd;
            session (fd);
            currentFd = -1;
            unixsocket::closeFd (fd);

            if (ready.exchange (false) && onReadyChanged)
                onReadyChanged (false);

            // Commands sent to a dead session never get an answer.
            std::map<int, Callback> lost;
            {
                std::lock_guard<std::mutex> guard (lock);
                lost.swap (inFlight);
            }
            for (auto& [id, cb] : lost)
                if (cb)
                    cb ({}, "QMP connection closed");

            wake.wait (retryIntervalMs);
        }
    }
}
