// MCU emulator protocol tests: framing with the real PktMgr, the Linux-side
// parser from input_events.cpp, and the request/response flows of the app.

#include "LinuxSideParser.h"
#include "emu/McuProtocol.h"

#include <contracts/dcl/dcl_contracts.h>
#include <juce_core/juce_core.h>

using namespace kickemu;
using DC = contracts::DeviceControlContracts;

namespace
{
    CPktMgr::HWMsg_t makeMsg (CPktMgr::enumPacketType type, int cmd, std::vector<int> data,
                              CPktMgr::enumPacketDir dir = CPktMgr::EN_PKT_REQUEST)
    {
        CPktMgr::HWMsg_t m;
        m.pktType = type;
        m.pktDir = dir;
        m.pktCmd = cmd;
        m.data = std::move (data);
        return m;
    }

    /** A protocol instance with its output captured and fed through the Linux parser. */
    struct Rig
    {
        PanelState state;
        McuProtocol proto { state, McuProtocol::Options() };
        LinuxSideParser linux;
        std::vector<std::vector<juce::uint8>> wire;
        std::vector<CPktMgr::HWMsg_t> received;   // what the Linux side dispatched

        Rig()
        {
            proto.transmit = [this] (const std::vector<juce::uint8>& bytes)
            {
                wire.push_back (bytes);
                auto msgs = linux.onSerialData (bytes);
                received.insert (received.end(), msgs.begin(), msgs.end());
            };
        }

        void fromHost (const CPktMgr::HWMsg_t& m)
        {
            auto bytes = McuProtocol::encodeHostFrame (m);
            proto.receive (bytes.data(), bytes.size());
        }
    };
}

class McuFramingTests : public juce::UnitTest
{
public:
    McuFramingTests() : juce::UnitTest ("MCU framing", "mcu") {}

    void runTest() override
    {
        beginTest ("Host frame layout and CRC");
        {
            auto bytes = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { 8, 1, 0 }));
            expectEquals ((int) bytes.size(), 6 + 3 + 1);
            expectEquals ((int) bytes[0], (int) 'M');
            expectEquals ((int) bytes[1], (int) 'T');
            expectEquals ((int) bytes[2], (int) CPktMgr::EN_PKT_TYPE_LED);
            expectEquals ((int) bytes[3], (int) CPktMgr::EN_PKT_REQUEST);
            expectEquals ((int) bytes[4], (int) DC::EN_LED_SET_SINGLE_LED);
            expectEquals ((int) bytes[5], 4); // data size + CRC byte
            std::vector<char> head (bytes.begin(), bytes.end() - 1);
            expectEquals ((int) bytes.back(), (int) CPktMgr::CrcGen (head));
        }

        beginTest ("CRC-8 polynomial 0x31, init 0xff");
        {
            // Reference value computed independently for "MT" 01 00 00 03 1f 01.
            std::vector<char> data { 'M', 'T', 1, 0, 0, 3, 0x1f, 1 };
            juce::uint8 crc = 0xff;
            for (auto c : data)
            {
                crc ^= (juce::uint8) c;
                for (int i = 0; i < 8; ++i)
                    crc = (crc & 0x80) ? (juce::uint8) ((crc << 1) ^ 0x31) : (juce::uint8) (crc << 1);
            }
            expectEquals ((int) CPktMgr::CrcGen (data), (int) crc);
        }

        beginTest ("MCU frames carry one trailer byte and pass the Linux parser");
        {
            Rig rig;
            rig.proto.sendButton (DC::EN_SHIFT, DC::BTN_PRESSED);
            expectEquals ((int) rig.wire.size(), 1);
            expectEquals ((int) rig.wire[0].size(), 6 + 2 + 1 + 1);
            expectEquals ((int) rig.wire[0].back(), 10);
            expectEquals ((int) rig.received.size(), 1);
            auto& m = rig.received[0];
            expectEquals ((int) m.pktType, (int) CPktMgr::EN_PKT_TYPE_BUTTON);
            expectEquals (m.pktCmd, (int) DC::EN_BTN_STATUS);
            expectEquals ((int) m.data.size(), 2);
            expectEquals (m.data[0], (int) DC::EN_SHIFT);
            expectEquals (m.data[1], (int) DC::BTN_PRESSED);
        }

        beginTest ("Without the trailer the Linux parser loses the packet");
        {
            PanelState state;
            McuProtocol::Options o;
            o.trailer.clear();
            McuProtocol proto (state, o);
            LinuxSideParser linux;
            std::vector<CPktMgr::HWMsg_t> got;
            proto.transmit = [&] (const std::vector<juce::uint8>& b) { auto m = linux.onSerialData (b); got.insert (got.end(), m.begin(), m.end()); };
            proto.sendButton (DC::EN_PLAY, DC::BTN_PRESSED);
            expectEquals ((int) got.size(), 0);
        }

        beginTest ("Back-to-back frames in one read are all dispatched");
        {
            Rig rig;
            std::vector<juce::uint8> burst;
            rig.proto.transmit = [&burst] (const std::vector<juce::uint8>& b) { burst.insert (burst.end(), b.begin(), b.end()); };
            rig.proto.sendPad (0, DC::BTN_PRESSED, 100, 0);
            rig.proto.sendPad (0, DC::BTN_RELEASED, 0, 0);
            rig.proto.sendKnob (DC::EN_KNOB_2, -3);
            rig.proto.sendSlider (42);
            auto msgs = rig.linux.onSerialData (burst);
            expectEquals ((int) msgs.size(), 4);
            if (msgs.size() == 4)
            {
                expect (msgs[0].pktType == CPktMgr::EN_PKT_TYPE_PAD && msgs[0].data == std::vector<int> { 0, 1, 100, 0 });
                expect (msgs[1].data == std::vector<int> { 0, 0, 0, 0 });
                expect (msgs[2].pktType == CPktMgr::EN_PKT_TYPE_KNOBS && msgs[2].pktCmd == DC::EN_KNOB_ROTARY);
                expect (msgs[2].data == std::vector<int> { DC::EN_KNOB_2, DC::EN_KNOB_ANTICLOCKWISE, 3 });
                expect (msgs[3].pktType == CPktMgr::EN_PKT_TYPE_SLIDER && msgs[3].data == std::vector<int> { 42 });
            }
        }

        beginTest ("Receiver resynchronises after garbage and split delivery");
        {
            Rig rig;
            auto frame = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_COLOR_MODE, { 7 }));
            std::vector<juce::uint8> stream { 0x00, 'M', 0x55, 'X' };
            stream.insert (stream.end(), frame.begin(), frame.end());
            // Deliver one byte at a time.
            for (auto b : stream)
                rig.proto.receive (&b, 1);
            expectEquals (rig.state.colourMode.load(), 7);
            expectEquals ((int) rig.state.packetsIn.load(), 1);
        }

        beginTest ("Corrupted CRC is rejected, the next frame still decodes");
        {
            Rig rig;
            auto bad = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_COLOR_MODE, { 2 }));
            bad.back() ^= 0x5a;
            auto good = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_COLOR_MODE, { 5 }));
            rig.proto.receive (bad.data(), bad.size());
            rig.proto.receive (good.data(), good.size());
            expectEquals (rig.state.colourMode.load(), 5);
            expect (rig.state.framingErrors.load() > 0);
        }

        beginTest ("Payload-less frame (size byte 1) is accepted");
        {
            Rig rig;
            rig.proto.connectionChanged (true, 0);
            rig.received.clear();
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_GET_SW_VER, {}));
            expectEquals ((int) rig.received.size(), 1);
            if (! rig.received.empty())
                expect (rig.received[0].data == std::vector<int> { 1, 0 });
        }

        beginTest ("Large step-sequencer frame (33 data bytes)");
        {
            Rig rig;
            std::vector<int> data { DC::StateOn };
            for (int i = 0; i < 16; ++i) data.push_back (i % 15);
            for (int i = 0; i < 16; ++i) data.push_back (i * 8);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_STEP_ALL_PAD, data));
            expectEquals (rig.state.pads[3].colourId.load(), 3);
            expectEquals (rig.state.pads[3].level.load(), 24);
            expectEquals ((int) rig.state.pads[15].mode.load(), (int) DC::StateOn);
        }
    }
};

class McuFlowTests : public juce::UnitTest
{
public:
    McuFlowTests() : juce::UnitTest ("MCU request/response flows", "mcu") {}

    void runTest() override
    {
        beginTest ("Comm init handshake as in kick_AppHome::handleCommand");
        {
            Rig rig;
            rig.proto.connectionChanged (true, 1000);
            rig.proto.tick (1000);
            expectEquals ((int) rig.received.size(), 1);
            // The app sees isSystemCommInit: SYSTEM / CHECK_COMM_STATUS / data[0] == 1
            expect (rig.received[0].pktType == CPktMgr::EN_PKT_TYPE_SYSTEM);
            expectEquals (rig.received[0].pktCmd, (int) DC::EN_SYS_CHECK_COMM_STATUS);
            expectEquals (rig.received[0].data[0], 1);

            // Repeats until answered.
            rig.proto.tick (1500);
            expectEquals ((int) rig.received.size(), 1);
            rig.proto.tick (2000);
            expectEquals ((int) rig.received.size(), 2);

            // App answers with COMM_INIT [EN_SYS_COMM_INIT]; MCU reports its version.
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_COMM_INIT, { DC::EN_SYS_COMM_INIT }));
            expect (rig.proto.getLinkState() == McuProtocol::LinkState::linked);
            expect (rig.state.hostLinked.load());
            expectEquals ((int) rig.received.size(), 3);
            expectEquals (rig.received[2].pktCmd, (int) DC::EN_SYS_GET_SW_VER);
            expect (rig.received[2].data == std::vector<int> { 1, 0 });
        }

        beginTest ("input_events connect() message links too");
        {
            Rig rig;
            rig.proto.connectionChanged (true, 0);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_COMM_INIT, { DC::EN_SYS_COMM_CONNECTED }));
            expect (rig.proto.getLinkState() == McuProtocol::LinkState::linked);
        }

        beginTest ("Keepalive and loss of the host");
        {
            Rig rig;
            rig.proto.connectionChanged (true, 0);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_COMM_INIT, { 2 }));
            rig.received.clear();
            auto now = juce::Time::getMillisecondCounter();
            rig.proto.tick (now + 2100);
            expectEquals ((int) rig.received.size(), 1);
            expectEquals (rig.received[0].pktCmd, (int) DC::EN_SYS_CHECK_COMM_STATUS);
            expectEquals (rig.received[0].data[0], 2);
            // No answer for a long time: back to comm init.
            rig.proto.tick (now + 20000);
            expect (rig.proto.getLinkState() == McuProtocol::LinkState::waitingForHost);
        }

        beginTest ("Software version request");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_GET_SW_VER, { 0 }));
            expectEquals ((int) rig.received.size(), 1);
            expectEquals (rig.received[0].pktCmd, (int) DC::EN_SYS_GET_SW_VER);
        }

        beginTest ("BPM (setTempoMessage)");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_BPM, { 120 >> 8, 120 & 0xff }));
            expectEquals (rig.state.bpm.load(), 120);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_BPM, { 300 >> 8, 300 & 0xff }));
            expectEquals (rig.state.bpm.load(), 300);
        }

        beginTest ("LEDs: setLEDONMessage / setLEDOFFMessage / blink / all");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_MAINMODE, DC::StateOn, 0 }));
            expectEquals ((int) rig.state.leds[DC::EN_MAINMODE].mode.load(), (int) PanelState::ledOn);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_MAINMODE, DC::StateOff, 0 }));
            expectEquals ((int) rig.state.leds[DC::EN_MAINMODE].mode.load(), (int) PanelState::ledOff);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_BLINK_SINGLE, { DC::EN_RECORD, 1 }));
            expectEquals ((int) rig.state.leds[DC::EN_RECORD].mode.load(), (int) PanelState::ledBlink);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_ALL, { DC::StateOn }));
            expectEquals ((int) rig.state.leds[DC::EN_ERASE].mode.load(), (int) PanelState::ledOn);
            expectEquals ((int) rig.state.leds[0].mode.load(), (int) PanelState::ledOn);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_ALL, { DC::StateOff }));
            expectEquals ((int) rig.state.leds[DC::EN_ERASE].mode.load(), (int) PanelState::ledOff);
        }

        beginTest ("Bank LEDs are exclusive (setBankMessage)");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_BANK_A, 1 }));
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_BANK_F, 1 }));
            expectEquals ((int) rig.state.leds[DC::EN_BANK_A].mode.load(), (int) PanelState::ledOff);
            expectEquals ((int) rig.state.leds[DC::EN_BANK_F].mode.load(), (int) PanelState::ledOn);
        }

        beginTest ("Transport LEDs are exclusive (setMediaControlMessage)");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_PLAY, DC::StateOn, 1 }));
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_STOP, DC::StateOn, 1 }));
            expectEquals ((int) rig.state.leds[DC::EN_PLAY].mode.load(), (int) PanelState::ledOff);
            expectEquals ((int) rig.state.leds[DC::EN_STOP].mode.load(), (int) PanelState::ledOn);
        }

        beginTest ("Pads: single, all single colour, all multi colour, colour+state");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_SINGLE_PAD, { 4, DC::StateOn, DC::PadColourAqua }));
            expectEquals ((int) rig.state.pads[4].mode.load(), (int) DC::StateOn);
            expectEquals (rig.state.pads[4].colourId.load(), (int) DC::PadColourAqua);

            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_ALL_SINGLE_COLOUR, { 0, DC::PadColourTrans })); // clearAllPadsMessage
            for (auto& p : rig.state.pads)
                expectEquals (p.colourId.load(), (int) DC::PadColourTrans);

            std::vector<int> multi { DC::StateOn };
            for (int i = 0; i < 16; ++i) multi.push_back (i);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_ALL_MULTI_COLOUR, multi));
            expectEquals (rig.state.pads[9].colourId.load(), 9);

            std::vector<int> cs;
            for (int i = 0; i < 16; ++i) cs.push_back ((i + 3) % 15);
            for (int i = 0; i < 16; ++i) cs.push_back (i % 2 == 0 ? DC::StateSampleAssigned : DC::StateSampleNotAssigned);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_COLOUR_STATE, cs));
            expectEquals (rig.state.pads[0].colourId.load(), 3);
            expectEquals ((int) rig.state.pads[0].mode.load(), (int) DC::StateSampleAssigned);
            expectEquals ((int) rig.state.pads[1].mode.load(), (int) DC::StateSampleNotAssigned);
        }

        beginTest ("Pads: blinking with two colours, settings, colour mode, aftertouch");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_BLINKING, { 2, DC::StateOnBlink, DC::PadColourRed, DC::PadColourBlue1 }));
            expect (rig.state.pads[2].blinkTwoColours.load());
            expectEquals (rig.state.pads[2].blinkOffColourId.load(), (int) DC::PadColourBlue1);

            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SETTINGS, { 16, 10, 64, 2 }));
            expectEquals (rig.state.padSettings[2].load(), 64);

            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_COLOR_MODE, { PanelState::modeOffDimFull }));
            expectEquals (rig.state.colourMode.load(), (int) PanelState::modeOffDimFull);

            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_AFTER_TOUCH, { 1 }));
            expect (rig.state.aftertouchEnabled.load());
        }

        beginTest ("Slider LEDs");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SLIDER, DC::EN_SLIDER_BAR_LED, { 50 }));
            expectEquals ((int) rig.state.sliderMode.load(), (int) PanelState::sliderBar);
            expectEquals (rig.state.sliderPercent.load(), 50);
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SLIDER, DC::EN_SLIDER_LEVEL_LED, { 75 }));
            expectEquals ((int) rig.state.sliderMode.load(), (int) PanelState::sliderLevel);
        }

        beginTest ("Pad press marks the pad pressed and is sent at once");
        {
            Rig rig;
            rig.proto.sendPad (7, DC::BTN_PRESSED, 99, 0);
            expect (rig.state.pads[7].pressed.load());
            expectEquals (rig.state.pads[7].pressVelocity.load(), 99);
            expectEquals ((int) rig.received.size(), 1);
            rig.proto.sendPad (7, DC::BTN_RELEASED, 0, 0);
            expect (! rig.state.pads[7].pressed.load());
        }

        beginTest ("Aftertouch is only sent when the app enabled it");
        {
            Rig rig;
            rig.proto.sendPadAftertouch (1, 100, 50);
            expectEquals ((int) rig.received.size(), 0);
            rig.state.aftertouchEnabled = true;
            rig.proto.sendPadAftertouch (1, 100, 50);
            expectEquals ((int) rig.received.size(), 1);
            if (! rig.received.empty())
                expect (rig.received[0].data == std::vector<int> { 1, 2, 100, 50 });
        }

        beginTest ("Reset from the host clears the panel");
        {
            Rig rig;
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { DC::EN_ERASE, 1, 0 }));
            rig.fromHost (makeMsg (CPktMgr::EN_PKT_TYPE_SYSTEM, DC::EN_SYS_RESET_GD, { 0 }));
            expectEquals ((int) rig.state.leds[DC::EN_ERASE].mode.load(), (int) PanelState::ledOff);
        }

        beginTest ("High packet rate: 10000 LED/pad updates");
        {
            Rig rig;
            std::vector<juce::uint8> stream;
            for (int i = 0; i < 5000; ++i)
            {
                auto a = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_PAD, DC::EN_PAD_SET_SINGLE_PAD, { i % 16, DC::StateOn, i % 15 }));
                auto b = McuProtocol::encodeHostFrame (makeMsg (CPktMgr::EN_PKT_TYPE_LED, DC::EN_LED_SET_SINGLE_LED, { i % 53, i % 2, 0 }));
                stream.insert (stream.end(), a.begin(), a.end());
                stream.insert (stream.end(), b.begin(), b.end());
            }
            auto start = juce::Time::getMillisecondCounterHiRes();
            for (size_t off = 0; off < stream.size(); off += 4096)
                rig.proto.receive (stream.data() + off, juce::jmin ((size_t) 4096, stream.size() - off));
            auto ms = juce::Time::getMillisecondCounterHiRes() - start;
            expectEquals ((int) rig.state.packetsIn.load(), 10000);
            expectEquals ((int) rig.state.framingErrors.load(), 0);
            logMessage ("10000 packets decoded in " + juce::String (ms, 2) + " ms");
            expect (ms < 500.0);
        }
    }
};

static McuFramingTests mcuFramingTests;
static McuFlowTests mcuFlowTests;
