#pragma once

#include "config/Config.h"
#include "emu/TerminalModel.h"
#include "emu/UartServer.h"

namespace kickemu
{
    /**
        A serial console UART (UART4, or UART2) on a socket, feeding a terminal
        model. Keyboard input from the terminal view goes back to the guest.
        Optionally mirrors raw output to another sink (stdout in headless mode).
    */
    class ConsoleBridge
    {
    public:
        ConsoleBridge (const Config& config, const juce::String& socketName);

        bool start (juce::String& error);
        void stop();

        void sendText (const juce::String& text);
        void sendBytes (const void* data, size_t size) { server.send (data, size); }

        TerminalModel& getModel() { return model; }
        bool isConnected() const { return server.isConnected(); }
        juce::String getName() const { return name; }

        std::function<void (const juce::uint8*, size_t)> onRawOutput;

        /** Off while a protocol (the guest agent on UART2) owns the port: its traffic is
            kept out of the terminal. */
        void setTerminalEnabled (bool on) { terminalEnabled = on; }

    private:
        const Config& config;
        juce::String name;
        UartServer server;
        TerminalModel model;
        std::atomic<bool> terminalEnabled { true };
    };
}
