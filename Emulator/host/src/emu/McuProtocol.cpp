#include "emu/McuProtocol.h"

#include <contracts/dcl/dcl_contracts.h>

namespace kickemu
{
    using DC = contracts::DeviceControlContracts;

    namespace
    {
        // PushByteToRxFifo keeps its CRC accumulator in a function-local static that
        // is shared by every CPktMgr instance, so frames are fed under one lock.
        std::mutex& pktMgrRxLock()
        {
            static std::mutex m;
            return m;
        }

        constexpr size_t headerSize = 6; // 'M' 'T' type dir cmd size

        juce::uint8 clampByte (int v) { return (juce::uint8) juce::jlimit (0, 255, v); }
    }

    McuProtocol::McuProtocol (PanelState& s, Options o)
        : state (s),
          options (std::move (o)),
          rxMgr (CPktMgr::EN_COM_CHANNEL_USB, rxFifo.data(), 255, rxTxFifo.data(), 255, 60),
          txMgr (CPktMgr::EN_COM_CHANNEL_USB, txFifoA.data(), 255, txFifoB.data(), 255, 60)
    {
        rxMgr.Init();
        txMgr.Init();
    }

    // ---------------------------------------------------------------------------------
    // Encoding

    std::vector<juce::uint8> McuProtocol::encodeHostFrame (const CPktMgr::HWMsg_t& msg)
    {
        // Same call sequence as HwSerialData::run() in input_events.cpp.
        static std::mutex lock;
        static std::array<juce::uint8, 256> a {}, b {};
        static CPktMgr mgr (CPktMgr::EN_COM_CHANNEL_USB, a.data(), 255, b.data(), 255, 60);

        std::lock_guard<std::mutex> guard (lock);
        auto copy = msg;
        std::vector<char> bytes;
        mgr.BuildAndTransmitPacket (copy, bytes);
        return { bytes.begin(), bytes.end() };
    }

    std::vector<juce::uint8> McuProtocol::encode (const CPktMgr::HWMsg_t& msg)
    {
        std::vector<char> bytes;
        {
            std::lock_guard<std::mutex> guard (txLock);
            auto copy = msg;
            txMgr.BuildAndTransmitPacket (copy, bytes);
        }
        std::vector<juce::uint8> out (bytes.begin(), bytes.end());
        out.insert (out.end(), options.trailer.begin(), options.trailer.end());
        return out;
    }

    void McuProtocol::send (const CPktMgr::HWMsg_t& msg)
    {
        auto bytes = encode (msg);
        state.packetsOut++;
        if (onPacket)
            onPacket (msg, false);
        if (transmit)
            transmit (bytes);
    }

    void McuProtocol::sendSystem (int cmd, std::vector<int> data)
    {
        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::EN_PKT_TYPE_SYSTEM;
        msg.pktDir = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd = cmd;
        msg.data = std::move (data);
        send (msg);
    }

    // ---------------------------------------------------------------------------------
    // Control surface events

    void McuProtocol::sendButton (int buttonId, int buttonState)
    {
        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::EN_PKT_TYPE_BUTTON;
        msg.pktDir = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd = DC::EN_BTN_STATUS;
        msg.data = { buttonId, buttonState };
        send (msg);
    }

    void McuProtocol::sendPad (int pad, int padState, int velocity, int aftertouch)
    {
        if (juce::isPositiveAndBelow (pad, PanelState::numPads))
        {
            auto& p = state.pads[(size_t) pad];
            p.pressed = padState == DC::BTN_PRESSED;
            if (padState == DC::BTN_PRESSED)
                p.pressVelocity = velocity;
            state.touch();
        }

        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::EN_PKT_TYPE_PAD;
        msg.pktDir = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd = DC::EN_PAD_STATE;
        msg.data = { pad, padState, clampByte (velocity), clampByte (aftertouch) };
        send (msg);
    }

    void McuProtocol::sendPadAftertouch (int pad, int velocity, int pressure)
    {
        if (! state.aftertouchEnabled.load())
            return;
        sendPad (pad, options.aftertouchState, velocity, pressure);
    }

    void McuProtocol::sendKnob (int knob, int steps)
    {
        if (steps == 0)
            return;

        // One packet per call; the rotation byte carries the step count.
        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::EN_PKT_TYPE_KNOBS;
        msg.pktDir = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd = DC::EN_KNOB_ROTARY;
        msg.data = { knob, steps > 0 ? (int) DC::EN_KNOB_CLOCKWISE : (int) DC::EN_KNOB_ANTICLOCKWISE,
                     clampByte (std::abs (steps)) };
        send (msg);
    }

    void McuProtocol::sendSlider (int percent)
    {
        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::EN_PKT_TYPE_SLIDER;
        msg.pktDir = CPktMgr::EN_PKT_REQUEST;
        msg.pktCmd = DC::EN_SLIDER_TOUCH_INPUT;
        msg.data = { juce::jlimit (0, 100, percent) };
        send (msg);
    }

    // ---------------------------------------------------------------------------------
    // Link state and timers

    void McuProtocol::connectionChanged (bool connected, juce::uint32 nowMs)
    {
        state.uartConnected = connected;
        state.hostLinked = false;
        linkState = connected ? LinkState::waitingForHost : LinkState::disconnected;
        versionSent = false;
        rxBuffer.clear();
        lastInitSent = nowMs - (juce::uint32) options.commInitIntervalMs; // send right away
        lastHeard = nowMs;
        state.touch();
        log (connected ? "UART1 connected, waiting for the host" : "UART1 disconnected");
    }

    void McuProtocol::tick (juce::uint32 nowMs)
    {
        switch (linkState.load())
        {
            case LinkState::waitingForHost:
                if (options.commInitIntervalMs > 0 && nowMs - lastInitSent >= (juce::uint32) options.commInitIntervalMs)
                {
                    lastInitSent = nowMs;
                    sendSystem (DC::EN_SYS_CHECK_COMM_STATUS, { 1 });
                }
                break;

            case LinkState::linked:
                if (options.keepaliveIntervalMs > 0)
                {
                    if (nowMs - lastKeepaliveSent >= (juce::uint32) options.keepaliveIntervalMs)
                    {
                        lastKeepaliveSent = nowMs;
                        sendSystem (DC::EN_SYS_CHECK_COMM_STATUS, { 2 });
                    }
                    if (nowMs - lastHeard > (juce::uint32) options.keepaliveIntervalMs * 4)
                    {
                        log ("Host stopped answering the keepalive, restarting the comm init");
                        linkState = LinkState::waitingForHost;
                        state.hostLinked = false;
                        versionSent = false;
                        state.touch();
                    }
                }
                break;

            case LinkState::disconnected:
                break;
        }
    }

    // ---------------------------------------------------------------------------------
    // Receive path

    void McuProtocol::receive (const juce::uint8* data, size_t size)
    {
        rxBuffer.insert (rxBuffer.end(), data, data + size);
        const auto now = juce::Time::getMillisecondCounter();

        size_t pos = 0;
        while (rxBuffer.size() - pos >= 2)
        {
            // Resynchronise on the "MT" start marker.
            if (rxBuffer[pos] != 'M' || rxBuffer[pos + 1] != 'T')
            {
                ++pos;
                state.framingErrors++;
                continue;
            }

            if (rxBuffer.size() - pos < headerSize)
                break;

            const int sizeByte = rxBuffer[pos + 5];
            const size_t frameLen = headerSize + (size_t) juce::jmax (1, sizeByte);
            if (rxBuffer.size() - pos < frameLen)
                break;

            const juce::uint8* frame = rxBuffer.data() + pos;
            CPktMgr::HWMsg_t msg;
            bool ok = false;

            if (sizeByte >= 2 || sizeByte == 0)
            {
                std::lock_guard<std::mutex> guard (pktMgrRxLock());
                rxMgr.ClearReceiver();
                for (size_t i = 0; i < frameLen; ++i)
                    rxMgr.PushByteToRxFifo (frame[i]);

                if (rxMgr.IsFrameRcvComplete())
                {
                    ok = true;
                    msg.pktType = rxMgr.GetRxPktType();
                    msg.pktDir = rxMgr.GetPktDir();
                    msg.pktCmd = rxMgr.GetRxCommand();
                    for (int i = 0; i < (int) rxMgr.GetRxFrameSize() - 1; ++i)
                        msg.data.push_back ((int) rxMgr.PopUint8FromRxFifo());
                }
                rxMgr.ClearReceiver();
            }
            else
            {
                // size == 1 (no payload): PktMgr's state machine waits for a data
                // byte that never comes, so check the CRC with its own CrcGen.
                std::vector<char> header (frame, frame + headerSize);
                if (CPktMgr::CrcGen (header) == frame[headerSize])
                {
                    ok = true;
                    msg.pktType = (CPktMgr::enumPacketType) frame[2];
                    msg.pktDir = (CPktMgr::enumPacketDir) frame[3];
                    msg.pktCmd = frame[4];
                }
            }

            if (! ok)
            {
                state.framingErrors++;
                ++pos; // skip this 'M' and look for the next frame start
                continue;
            }

            pos += frameLen;
            state.packetsIn++;
            if (onPacket)
                onPacket (msg, true);
            handle (msg, now);
        }

        rxBuffer.erase (rxBuffer.begin(), rxBuffer.begin() + (std::ptrdiff_t) pos);

        // A frame is at most 6 + 255 bytes; anything bigger is garbage.
        if (rxBuffer.size() > 1024)
            rxBuffer.clear();
    }

    void McuProtocol::handle (const CPktMgr::HWMsg_t& msg, juce::uint32 nowMs)
    {
        lastHeard = nowMs;
        switch ((int) msg.pktType)
        {
            case CPktMgr::EN_PKT_TYPE_SYSTEM: handleSystem (msg); break;
            case CPktMgr::EN_PKT_TYPE_LED: handleLed (msg); break;
            case CPktMgr::EN_PKT_TYPE_PAD: handlePad (msg); break;
            case CPktMgr::EN_PKT_TYPE_SLIDER: handleSlider (msg); break;
            default:
                log ("Unhandled packet type " + juce::String ((int) msg.pktType) + " cmd " + juce::String (msg.pktCmd));
                break;
        }
        state.touch();
    }

    void McuProtocol::handleSystem (const CPktMgr::HWMsg_t& msg)
    {
        auto d = [&msg] (size_t i, int fallback = 0) { return i < msg.data.size() ? msg.data[i] : fallback; };

        switch (msg.pktCmd)
        {
            case DC::EN_SYS_COMM_INIT:
                if (linkState != LinkState::linked)
                    log ("Host linked (COMM_INIT)");
                linkState = LinkState::linked;
                state.hostLinked = true;
                lastKeepaliveSent = juce::Time::getMillisecondCounter();
                if (! versionSent)
                {
                    versionSent = true;
                    sendSystem (DC::EN_SYS_GET_SW_VER, { options.swMajor, options.swMinor });
                }
                break;

            case DC::EN_SYS_CHECK_COMM_STATUS:
                if (linkState != LinkState::linked && d (0) == 2)
                {
                    linkState = LinkState::linked;
                    state.hostLinked = true;
                }
                break;

            case DC::EN_SYS_GET_SW_VER:
                sendSystem (DC::EN_SYS_GET_SW_VER, { options.swMajor, options.swMinor });
                break;

            case DC::EN_SYS_BPM:
                state.bpm = (d (0) << 8) | d (1);
                break;

            case DC::EN_SYS_RESET_GD:
                log ("Reset requested by the host");
                state.reset();
                linkState = LinkState::waitingForHost;
                state.hostLinked = false;
                versionSent = false;
                break;

            case DC::EN_SYS_SHUTDOWN:
                log ("Shutdown requested by the host");
                state.reset();
                break;

            case DC::EN_SYS_SW_UPDATE:
                log ("Control surface firmware update request (code " + juce::String (d (0)) + "): not applicable to the emulated MCU");
                break;

            default:
                log ("System command " + juce::String (msg.pktCmd) + " ignored");
                break;
        }
    }

    namespace
    {
        // PadButtonState values used for LEDs: 0 off, 1 on, 4/5 blink.
        juce::uint8 ledModeFromState (int s)
        {
            switch (s)
            {
                case DC::StateOff: return PanelState::ledOff;
                case DC::StateOnBlink:
                case DC::StateOffBlink: return PanelState::ledBlink;
                default: return PanelState::ledOn;
            }
        }
    }

    void McuProtocol::handleLed (const CPktMgr::HWMsg_t& msg)
    {
        auto d = [&msg] (size_t i, int fallback = 0) { return i < msg.data.size() ? msg.data[i] : fallback; };

        auto setLed = [this] (int id, juce::uint8 mode, int colour)
        {
            if (! juce::isPositiveAndBelow (id, PanelState::numLeds))
                return;
            auto& led = state.leds[(size_t) id];
            led.mode = mode;
            led.colourId = colour;
            led.addressed = true;
        };

        switch (msg.pktCmd)
        {
            case DC::EN_LED_SET_SINGLE_LED:
            {
                const int id = d (0);
                const auto mode = ledModeFromState (d (1));
                setLed (id, mode, d (2));

                // Bank and transport LEDs are exclusive on the device: the app only
                // ever switches the new one on (setBankMessage, setMediaControlMessage).
                if (mode != PanelState::ledOff)
                {
                    for (auto& group : options.ledRadioGroups)
                    {
                        if (std::find (group.begin(), group.end(), id) == group.end())
                            continue;
                        for (int other : group)
                            if (other != id)
                                setLed (other, PanelState::ledOff, 0);
                    }
                }
                break;
            }

            case DC::EN_LED_BLINK_SINGLE:
                setLed (d (0), d (1) != DC::StateOff ? PanelState::ledBlink : PanelState::ledOff, d (2));
                break;

            case DC::EN_LED_SET_ALL:
                if (msg.data.size() <= 2)
                {
                    for (int id = 0; id < DC::SIZE; ++id)
                        setLed (id, ledModeFromState (d (0)), d (1));
                }
                else
                {
                    for (int id = 0; id < (int) msg.data.size() && id < PanelState::numLeds; ++id)
                        setLed (id, ledModeFromState (msg.data[(size_t) id]), 0);
                }
                break;

            default:
                log ("LED command " + juce::String (msg.pktCmd) + " ignored");
                break;
        }
    }

    void McuProtocol::handlePad (const CPktMgr::HWMsg_t& msg)
    {
        const auto& v = msg.data;
        auto d = [&v] (size_t i, int fallback = 0) { return i < v.size() ? v[i] : fallback; };

        auto setPad = [this] (int pad, int mode, int colour, int level = 127)
        {
            auto apply = [&] (PanelState::Pad& p)
            {
                p.mode = (juce::uint8) mode;
                p.colourId = colour;
                p.level = level;
                p.blinkTwoColours = false;
            };
            if (pad == DC::EN_ALL_PADS)
            {
                for (auto& p : state.pads)
                    apply (p);
            }
            else if (juce::isPositiveAndBelow (pad, PanelState::numPads))
            {
                apply (state.pads[(size_t) pad]);
            }
        };

        switch (msg.pktCmd)
        {
            case DC::EN_PAD_SET_SINGLE_PAD:
                setPad (d (0), d (1), d (2, 15));
                break;

            case DC::EN_PAD_SET_ALL_SINGLE_COLOUR:
                setPad (DC::EN_ALL_PADS, d (0), d (1, 15));
                break;

            case DC::EN_PAD_SET_ALL_MULTI_COLOUR:
                for (int i = 0; i < PanelState::numPads && (size_t) (i + 1) < v.size(); ++i)
                    setPad (i, d (0), v[(size_t) i + 1]);
                break;

            case DC::EN_PAD_SET_COLOUR_STATE:
            {
                // [colour 0..n-1, state 0..n-1] for pads 0..n-1 (setPadColourAndStateInfoMessage).
                const int n = (int) v.size() / 2;
                for (int i = 0; i < n && i < PanelState::numPads; ++i)
                    setPad (i, v[(size_t) (n + i)], v[(size_t) i]);
                break;
            }

            case DC::EN_PAD_SETTINGS:
                for (size_t i = 0; i < state.padSettings.size(); ++i)
                    state.padSettings[i] = d (i);
                break;

            case DC::EN_PAD_COLOR_MODE:
                state.colourMode = d (0);
                break;

            case DC::EN_PAD_SET_AFTER_TOUCH:
                state.aftertouchEnabled = d (0) != 0;
                break;

            case DC::EN_PAD_SET_BLINKING:
            {
                const int pad = d (0);
                setPad (pad, d (1), d (2, 15));
                const int offColour = d (3, 15);
                auto applyOff = [offColour] (PanelState::Pad& p)
                {
                    p.blinkOffColourId = offColour;
                    p.blinkTwoColours = offColour != 15;
                };
                if (pad == DC::EN_ALL_PADS)
                    for (auto& p : state.pads)
                        applyOff (p);
                else if (juce::isPositiveAndBelow (pad, PanelState::numPads))
                    applyOff (state.pads[(size_t) pad]);
                break;
            }

            case DC::EN_PAD_SET_STEP_SINGLE_PAD:
                setPad (d (0), d (1), d (2, 15), d (3, 127));
                break;

            case DC::EN_PAD_SET_STEP_ALL_PAD:
                // [state, colour x16, velocity x16]
                for (int i = 0; i < PanelState::numPads; ++i)
                    setPad (i, d (0), d ((size_t) (1 + i), 15), d ((size_t) (17 + i), 127));
                break;

            default:
                log ("Pad command " + juce::String (msg.pktCmd) + " ignored");
                break;
        }
    }

    void McuProtocol::handleSlider (const CPktMgr::HWMsg_t& msg)
    {
        auto d = [&msg] (size_t i, int fallback = 0) { return i < msg.data.size() ? msg.data[i] : fallback; };

        switch (msg.pktCmd)
        {
            case DC::EN_SLIDER_BAR_LED:
                state.sliderMode = PanelState::sliderBar;
                state.sliderPercent = d (0);
                break;
            case DC::EN_SLIDER_LEVEL_LED:
                state.sliderMode = PanelState::sliderLevel;
                state.sliderPercent = d (0);
                break;
            case DC::EN_SLIDER_INDIVIDUAL_LED:
            {
                // [led index, on/off]
                auto mask = state.sliderMask.load();
                const int index = d (0);
                if (juce::isPositiveAndBelow (index, 32))
                    mask = d (1) != 0 ? (mask | (1u << index)) : (mask & ~(1u << index));
                state.sliderMask = mask;
                state.sliderMode = PanelState::sliderIndividual;
                break;
            }
            default:
                log ("Slider command " + juce::String (msg.pktCmd) + " ignored");
                break;
        }
    }
}
