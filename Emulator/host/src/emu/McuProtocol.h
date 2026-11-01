#pragma once

#include "emu/PanelState.h"

#include <PktMgr/PktMgr.hpp>
#include <functional>
#include <mutex>
#include <vector>

namespace kickemu
{
    /**
        The MCU side of the KICK control-surface protocol, independent of any
        transport.

        Framing is done by the real CPktMgr from ComProtocol/PktMgr (linked in, not
        reimplemented). Message semantics follow Kick/modules/contracts/dcl and the
        Linux side in Kick/modules/input_events/input_events.cpp:

        - Linux -> MCU: plain PktMgr frames ("MT", type, dir, cmd, size, data, crc).
        - MCU -> Linux: the same frame followed by a trailer byte. input_events.cpp
          reads data[5] + 7 bytes per packet and feeds all but the last one into
          PktMgr, so the MCU must append exactly one extra byte (configurable).
        - Only REQUEST packets from the MCU are dispatched by Linux, so events and
          system messages are sent as requests.

        Handshake (see kick_AppHome.cpp): the MCU sends SYSTEM/CHECK_COMM_STATUS [1]
        until the app answers with SYSTEM/COMM_INIT, then reports its version with
        SYSTEM/GET_SW_VER [major, minor] and sends CHECK_COMM_STATUS [2] as a
        keepalive, which the app echoes.
    */
    class McuProtocol
    {
    public:
        struct Options
        {
            std::vector<juce::uint8> trailer { 10 };
            int commInitIntervalMs = 1000;
            int keepaliveIntervalMs = 2000;
            int swMajor = 1, swMinor = 0;
            int aftertouchState = 2;          // pad state value used for aftertouch updates

            // LEDs that are mutually exclusive on the device (banks, transport):
            // switching one on switches the others in its group off.
            std::vector<std::vector<int>> ledRadioGroups { { 0, 1, 2, 3, 4, 5, 6, 7 }, { 44, 45, 46 } };
        };

        enum class LinkState { disconnected, waitingForHost, linked };

        McuProtocol (PanelState& state, Options options);

        /** Where encoded MCU -> Linux bytes go (the UART socket). */
        std::function<void (const std::vector<juce::uint8>&)> transmit;

        /** Every decoded packet, both directions (incoming = from Linux). */
        std::function<void (const CPktMgr::HWMsg_t&, bool incoming)> onPacket;

        /** Free-text protocol events for the log. */
        std::function<void (const juce::String&)> onLog;

        // --- transport side -------------------------------------------------------
        void connectionChanged (bool connected, juce::uint32 nowMs);
        void receive (const juce::uint8* data, size_t size);
        void tick (juce::uint32 nowMs);

        // --- control surface events (any thread) ----------------------------------
        void sendButton (int buttonId, int state);
        void sendPad (int pad, int state, int velocity, int aftertouch);
        void sendPadAftertouch (int pad, int velocity, int pressure);
        void sendKnob (int knob, int steps);           // signed steps, + = clockwise
        void sendSlider (int percent);

        /** Encodes a message exactly as the MCU puts it on the wire (with trailer). */
        std::vector<juce::uint8> encode (const CPktMgr::HWMsg_t& msg);

        /** Encodes a Linux -> MCU frame (no trailer), as input_events.cpp does. Tests use this. */
        static std::vector<juce::uint8> encodeHostFrame (const CPktMgr::HWMsg_t& msg);

        LinkState getLinkState() const { return linkState; }

    private:
        void handle (const CPktMgr::HWMsg_t& msg, juce::uint32 nowMs);
        void handleSystem (const CPktMgr::HWMsg_t& msg);
        void handleLed (const CPktMgr::HWMsg_t& msg);
        void handlePad (const CPktMgr::HWMsg_t& msg);
        void handleSlider (const CPktMgr::HWMsg_t& msg);
        void send (const CPktMgr::HWMsg_t& msg);
        void sendSystem (int cmd, std::vector<int> data);
        void log (const juce::String& text) { if (onLog) onLog (text); }

        PanelState& state;
        Options options;

        // Receive path (socket thread only).
        std::vector<juce::uint8> rxBuffer;
        std::array<juce::uint8, 256> rxFifo {}, rxTxFifo {};
        CPktMgr rxMgr;

        // Transmit path (any thread).
        std::mutex txLock;
        std::array<juce::uint8, 256> txFifoA {}, txFifoB {};
        CPktMgr txMgr;

        std::atomic<LinkState> linkState { LinkState::disconnected };
        juce::uint32 lastInitSent = 0, lastKeepaliveSent = 0, lastHeard = 0;
        bool versionSent = false;
    };
}
