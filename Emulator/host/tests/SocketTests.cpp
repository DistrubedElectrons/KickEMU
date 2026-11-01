// End-to-end tests over real unix sockets: the MCU emulator with a client that
// plays QEMU's chardev, and the RFB/QMP clients against small fake servers.

#include "LinuxSideParser.h"
#include "emu/McuEmulator.h"
#include "emu/QmpClient.h"
#include "emu/RfbClient.h"
#include "emu/UnixSocket.h"

#include <contracts/dcl/dcl_contracts.h>
#include <thread>

using namespace kickemu;
using DC = contracts::DeviceControlContracts;

namespace
{
    juce::File makeRunDir()
    {
        // Short path: unix socket paths are limited to 104 bytes on macOS.
        auto dir = juce::File ("/tmp").getChildFile ("kemu-t" + juce::String (juce::Random::getSystemRandom().nextInt (100000)));
        dir.createDirectory();
        return dir;
    }

    std::vector<juce::uint8> readFor (int fd, int ms)
    {
        std::vector<juce::uint8> out;
        auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < end)
        {
            if (unixsocket::waitReadable (fd, 20) > 0)
            {
                juce::uint8 buf[1024];
                int n = unixsocket::readSome (fd, buf, sizeof (buf));
                if (n <= 0)
                    break;
                out.insert (out.end(), buf, buf + n);
            }
        }
        return out;
    }

    template <typename Pred>
    bool waitUntil (Pred pred, int ms)
    {
        auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (! pred())
        {
            if (juce::Time::getMillisecondCounter() > end)
                return false;
            juce::Thread::sleep (5);
        }
        return true;
    }
}

class McuSocketTests : public juce::UnitTest
{
public:
    McuSocketTests() : juce::UnitTest ("MCU over a unix socket", "sockets") {}

    void runTest() override
    {
        auto runDir = makeRunDir();
        Config config;
        config.loadFromString ("{}", juce::File());
        config.set ("run_dir", runDir.getFullPathName());
        config.set ("host.mcu.comm_init_interval_ms", 50);

        PanelState state;
        McuEmulator mcu (state, config);
        juce::String error;

        beginTest ("Listening socket is created before QEMU connects");
        expect (mcu.start (error), error);
        expect (runDir.getChildFile ("mcu.sock").exists());

        beginTest ("QEMU-side client receives comm init and drives LEDs");
        {
            int fd = unixsocket::connectTo (runDir.getChildFile ("mcu.sock").getFullPathName(), error);
            expect (fd >= 0, error);
            expect (waitUntil ([&] { return state.uartConnected.load(); }, 1000));

            LinuxSideParser linux;
            auto bytes = readFor (fd, 150);
            auto msgs = linux.onSerialData (bytes);
            expect (! msgs.empty());
            if (! msgs.empty())
                expectEquals (msgs[0].pktCmd, (int) DC::EN_SYS_CHECK_COMM_STATUS);

            CPktMgr::HWMsg_t init;
            init.pktType = CPktMgr::EN_PKT_TYPE_SYSTEM;
            init.pktCmd = DC::EN_SYS_COMM_INIT;
            init.data = { DC::EN_SYS_COMM_INIT };
            auto frame = McuProtocol::encodeHostFrame (init);
            unixsocket::writeAll (fd, frame.data(), frame.size());
            expect (waitUntil ([&] { return state.hostLinked.load(); }, 1000));

            CPktMgr::HWMsg_t led;
            led.pktType = CPktMgr::EN_PKT_TYPE_LED;
            led.pktCmd = DC::EN_LED_SET_SINGLE_LED;
            led.data = { DC::EN_SHIFT, DC::StateOn, 0 };
            frame = McuProtocol::encodeHostFrame (led);
            unixsocket::writeAll (fd, frame.data(), frame.size());
            expect (waitUntil ([&] { return state.leds[DC::EN_SHIFT].mode.load() == PanelState::ledOn; }, 1000));

            // A pad press from the UI arrives at the guest side.
            readFor (fd, 30); // drain version / keepalive
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            mcu.protocol().sendPad (5, DC::BTN_PRESSED, 111, 0);
            std::vector<juce::uint8> got;
            bool found = false;
            while (! found && juce::Time::getMillisecondCounterHiRes() - t0 < 1000)
            {
                if (unixsocket::waitReadable (fd, 1) <= 0)
                    continue;
                juce::uint8 buf[256];
                int n = unixsocket::readSome (fd, buf, sizeof (buf));
                got.insert (got.end(), buf, buf + juce::jmax (0, n));
                // Frames are self-delimiting: look for the complete pad frame.
                for (size_t i = 0; i + 12 <= got.size(); ++i)
                    if (got[i] == 'M' && got[i + 1] == 'T' && got[i + 2] == CPktMgr::EN_PKT_TYPE_PAD && got[i + 5] == 5)
                    {
                        std::vector<juce::uint8> padFrame (got.begin() + (long) i, got.begin() + (long) i + 12);
                        for (auto& m : linux.onSerialData (padFrame))
                            found |= m.data == std::vector<int> { 5, 1, 111, 0 };
                    }
            }
            expect (found);
            auto latency = juce::Time::getMillisecondCounterHiRes() - t0;
            logMessage ("pad press reached the socket in " + juce::String (latency, 2) + " ms");

            unixsocket::closeFd (fd);
            expect (waitUntil ([&] { return ! state.uartConnected.load(); }, 1000));
        }

        beginTest ("QEMU can reconnect");
        {
            int fd = unixsocket::connectTo (runDir.getChildFile ("mcu.sock").getFullPathName(), error);
            expect (fd >= 0, "reconnect failed: " + error);
            expect (waitUntil ([&] { return state.uartConnected.load(); }, 1000), "no connection after reconnect");
            unixsocket::closeFd (fd);
        }

        mcu.stop();
        expect (! runDir.getChildFile ("mcu.sock").exists(), "socket file left behind");
        runDir.deleteRecursively();
    }
};

class RfbQmpSocketTests : public juce::UnitTest
{
public:
    RfbQmpSocketTests() : juce::UnitTest ("RFB and QMP clients", "sockets") {}

    void runTest() override
    {
        auto runDir = makeRunDir();
        juce::String error;

        beginTest ("RFB handshake, pixel format, raw and copyrect updates");
        {
            auto path = runDir.getChildFile ("vnc.sock");
            int lfd = unixsocket::listenAt (path.getFullPathName(), error);
            expect (lfd >= 0, error);

            std::vector<juce::uint8> clientBytes;
            std::thread server ([&]
            {
                if (unixsocket::waitReadable (lfd, 3000) <= 0) return;
                int fd = unixsocket::acceptClient (lfd);
                auto readN = [&] (size_t n)
                {
                    std::vector<juce::uint8> b (n);
                    size_t got = 0;
                    while (got < n && unixsocket::waitReadable (fd, 2000) > 0)
                    {
                        int r = unixsocket::readSome (fd, b.data() + got, (int) (n - got));
                        if (r <= 0) break;
                        got += (size_t) r;
                    }
                    clientBytes.insert (clientBytes.end(), b.begin(), b.end());
                    return b;
                };
                unixsocket::writeAll (fd, "RFB 003.008\n", 12);
                readN (12);
                juce::uint8 sec[] = { 1, 1 };
                unixsocket::writeAll (fd, sec, 2);
                readN (1);
                juce::uint8 ok[] = { 0, 0, 0, 0 };
                unixsocket::writeAll (fd, ok, 4);
                readN (1); // shared flag
                juce::uint8 init[24 + 4] = { 0, 4, 0, 2 }; // 4x2
                init[20] = 0; init[21] = 0; init[22] = 0; init[23] = 4;
                std::memcpy (init + 24, "test", 4);
                unixsocket::writeAll (fd, init, sizeof (init));
                readN (20); // SetPixelFormat
                readN (4 + 12); // SetEncodings (3)
                readN (10); // FramebufferUpdateRequest

                // Raw 2x1 at (1,0): blue and red pixels in X8R8G8B8 little endian.
                juce::uint8 upd[] = { 0, 0, 0, 2,
                                      0, 1, 0, 0, 0, 2, 0, 1, 0, 0, 0, 0,
                                      0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
                                      // CopyRect 1x1 to (0,1) from (2,0)
                                      0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 2, 0, 0 };
                unixsocket::writeAll (fd, upd, sizeof (upd));
                readN (10); // incremental request
                juce::Thread::sleep (100);
                unixsocket::closeFd (fd);
            });

            SharedFramebuffer fb;
            RfbClient client (fb);
            client.start (path, 50);
            expect (waitUntil ([&] { return client.updates.load() >= 1; }, 3000));
            {
                std::lock_guard<std::mutex> g (fb.lock);
                expectEquals (fb.image.getWidth(), 4);
                expectEquals (fb.image.getHeight(), 2);
                expect (fb.image.getPixelAt (1, 0) == juce::Colour (0xff0000ffu));
                expect (fb.image.getPixelAt (2, 0) == juce::Colour (0xffff0000u));
                expect (fb.image.getPixelAt (0, 1) == juce::Colour (0xffff0000u));
                expect (fb.image.getPixelAt (0, 0) == juce::Colours::black);
            }
            server.join();
            client.stop();
            unixsocket::closeFd (lfd);

            // Client version, then SetPixelFormat: 32 bpp, depth 24, little endian, true colour, shifts 16/8/0.
            expect (clientBytes.size() >= 12 + 1 + 1 + 20);
            if (clientBytes.size() >= 34)
            {
                expect (std::memcmp (clientBytes.data(), "RFB 003.008\n", 12) == 0);
                const juce::uint8* pf = clientBytes.data() + 14;
                expectEquals ((int) pf[0], 0);
                expectEquals ((int) pf[4], 32);
                expectEquals ((int) pf[6], 0);
                expectEquals ((int) pf[7], 1);
                expectEquals ((int) pf[14], 16);
                expectEquals ((int) pf[15], 8);
                expectEquals ((int) pf[16], 0);
            }
        }

        beginTest ("QMP capabilities, command ids and events");
        {
            auto path = runDir.getChildFile ("qmp.sock");
            int lfd = unixsocket::listenAt (path.getFullPathName(), error);
            juce::StringArray received;

            std::thread server ([&]
            {
                if (unixsocket::waitReadable (lfd, 3000) <= 0) return;
                int fd = unixsocket::acceptClient (lfd);
                auto greet = juce::String (R"({"QMP":{"version":{},"capabilities":[]}})") + "\r\n";
                unixsocket::writeAll (fd, greet.toRawUTF8(), greet.getNumBytesAsUTF8());
                juce::String pending;
                for (int lines = 0; lines < 2 && unixsocket::waitReadable (fd, 2000) > 0;)
                {
                    char buf[512];
                    int n = unixsocket::readSome (fd, buf, sizeof (buf));
                    if (n <= 0) break;
                    pending += juce::String::fromUTF8 (buf, n);
                    int nl;
                    while ((nl = pending.indexOfChar ('\n')) >= 0)
                    {
                        auto line = pending.substring (0, nl);
                        pending = pending.substring (nl + 1);
                        received.add (line);
                        ++lines;
                        juce::var msg;
                        juce::JSON::parse (line, msg);
                        auto reply = "{\"return\":{},\"id\":" + msg["id"].toString() + "}\n";
                        if (lines == 2)
                            reply = R"({"event":"STOP","data":{}})" "\n" + reply;
                        unixsocket::writeAll (fd, reply.toRawUTF8(), reply.getNumBytesAsUTF8());
                    }
                }
                juce::Thread::sleep (200);
                unixsocket::closeFd (fd);
            });

            QmpClient qmp;
            std::atomic<bool> replied { false }, gotEvent { false };
            qmp.onEvent = [&] (const juce::String& name, const juce::var&) { gotEvent = name == "STOP"; };
            qmp.execute ("input-send-event", juce::JSON::parse (R"({"events":[]})"),
                         [&] (const juce::var&, const juce::String& err) { replied = err.isEmpty(); });
            qmp.start (path, 50);
            expect (waitUntil ([&] { return replied.load(); }, 3000));
            expect (waitUntil ([&] { return gotEvent.load(); }, 1000));
            server.join();
            qmp.stop();
            unixsocket::closeFd (lfd);

            expectEquals (received.size(), 2);
            if (received.size() == 2)
            {
                expect (received[0].contains ("qmp_capabilities"));
                expect (received[1].contains ("input-send-event"));
            }
        }

        runDir.deleteRecursively();
    }
};

static McuSocketTests mcuSocketTests;
static RfbQmpSocketTests rfbQmpSocketTests;
