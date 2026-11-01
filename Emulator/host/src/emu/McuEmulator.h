#pragma once

#include "config/Config.h"
#include "emu/McuProtocol.h"
#include "emu/PanelState.h"
#include "emu/UartServer.h"

#include <condition_variable>
#include <deque>

namespace kickemu
{
    /**
        The emulated control-surface MCU on the UART1 socket (mcu.sock).

        Glues McuProtocol to a UartServer. Outgoing frames go through a small
        queue so each frame leaves in one write and frames can be spaced like on
        the real 230400 baud link (host.mcu.tx_frame_gap_us).
    */
    class McuEmulator : private juce::Thread
    {
    public:
        McuEmulator (PanelState& state, const Config& config);
        ~McuEmulator() override;

        bool start (juce::String& error);
        void stop();

        McuProtocol& protocol() { return *proto; }
        UartServer& uart() { return server; }

        /** Protocol log lines, called on the socket thread. */
        std::function<void (const juce::String&)> onLog;

        /** Optional packet monitor, called on the thread that produced the packet. */
        std::function<void (const CPktMgr::HWMsg_t&, bool incoming)> onPacket;

        static McuProtocol::Options optionsFromConfig (const Config& config);

    private:
        void run() override;
        void queueFrame (const std::vector<juce::uint8>& bytes);

        PanelState& state;
        const Config& config;
        UartServer server { "mcu" };
        std::unique_ptr<McuProtocol> proto;

        std::mutex queueLock;
        std::condition_variable queueSignal;
        std::deque<std::vector<juce::uint8>> txQueue;
        int frameGapUs = 0;
    };
}
