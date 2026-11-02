#pragma once

#include "automation/McpServer.h"

#include <list>
#include <thread>

namespace kickemu
{
    /**
        Serves the MCP protocol on a unix socket in the run directory
        (automation.sock), so scripts and agents can drive a running emulator,
        including the GUI app: `socat STDIO UNIX:<run_dir>/automation.sock` turns it
        into a stdio MCP server. One thread per client, so a long wait in one
        client does not stall another.
    */
    class AutomationSocket
    {
    public:
        explicit AutomationSocket (McpServer& server);
        ~AutomationSocket();

        bool start (const juce::File& socketPath, juce::String& error);
        void stop();

        static constexpr int maxClients = 8;

    private:
        struct Client
        {
            int fd = -1;
            std::thread thread;
            std::atomic<bool> finished { false };
        };

        void acceptLoop();
        void serveClient (Client& client);
        void reapFinished();

        McpServer& server;
        juce::File path;
        int listenFd = -1;
        int wakePipe[2] { -1, -1 };
        std::atomic<bool> stopping { false };
        std::thread acceptThread;
        std::mutex clientsLock;
        std::list<Client> clients;
    };
}
