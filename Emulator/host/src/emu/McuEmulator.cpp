#include "emu/McuEmulator.h"

#include <thread>

namespace kickemu
{
    McuProtocol::Options McuEmulator::optionsFromConfig (const Config& config)
    {
        McuProtocol::Options o;
        if (config.has ("host.mcu.frame_trailer"))
        {
            o.trailer.clear();
            for (auto b : config.getIntArray ("host.mcu.frame_trailer"))
                o.trailer.push_back ((juce::uint8) b);
        }
        o.commInitIntervalMs = config.getInt ("host.mcu.comm_init_interval_ms", o.commInitIntervalMs);
        o.keepaliveIntervalMs = config.getInt ("host.mcu.keepalive_interval_ms", o.keepaliveIntervalMs);
        auto version = config.getIntArray ("host.mcu.sw_version");
        if (version.size() >= 2)
        {
            o.swMajor = version[0];
            o.swMinor = version[1];
        }
        o.aftertouchState = config.getInt ("host.mcu.aftertouch_state", o.aftertouchState);

        if (auto* groups = config.get ("host.mcu.led_radio_groups").getArray())
        {
            o.ledRadioGroups.clear();
            for (auto& g : *groups)
            {
                std::vector<int> ids;
                if (auto* a = g.getArray())
                    for (auto& id : *a)
                        ids.push_back ((int) id);
                o.ledRadioGroups.push_back (ids);
            }
        }
        return o;
    }

    McuEmulator::McuEmulator (PanelState& s, const Config& c)
        : juce::Thread ("mcu tx"), state (s), config (c)
    {
        proto = std::make_unique<McuProtocol> (state, optionsFromConfig (config));
        frameGapUs = config.getInt ("host.mcu.tx_frame_gap_us", 0);

        proto->transmit = [this] (const std::vector<juce::uint8>& bytes) { queueFrame (bytes); };
        proto->onLog = [this] (const juce::String& text) { if (onLog) onLog (text); };
        proto->onPacket = [this] (const CPktMgr::HWMsg_t& msg, bool incoming) { if (onPacket) onPacket (msg, incoming); };

        server.onData = [this] (const juce::uint8* data, size_t size) { proto->receive (data, size); };
        server.onConnectionChanged = [this] (bool connected)
        {
            proto->connectionChanged (connected, juce::Time::getMillisecondCounter());
        };
        server.onTick = [this] { proto->tick (juce::Time::getMillisecondCounter()); };
        server.tickIntervalMs = 10;
    }

    McuEmulator::~McuEmulator() { stop(); }

    bool McuEmulator::start (juce::String& error)
    {
        if (! server.start (config.getSocketPath ("mcu"), error))
            return false;
        startThread (juce::Thread::Priority::high);
        return true;
    }

    void McuEmulator::stop()
    {
        signalThreadShouldExit();
        queueSignal.notify_all();
        stopThread (1000);
        server.stop();
    }

    void McuEmulator::queueFrame (const std::vector<juce::uint8>& bytes)
    {
        if (frameGapUs <= 0)
        {
            // No pacing: write straight from the caller's thread (lowest latency).
            server.send (bytes.data(), bytes.size());
            return;
        }
        {
            std::lock_guard<std::mutex> lock (queueLock);
            txQueue.push_back (bytes);
        }
        queueSignal.notify_one();
    }

    void McuEmulator::run()
    {
        while (! threadShouldExit())
        {
            std::vector<juce::uint8> frame;
            {
                std::unique_lock<std::mutex> lock (queueLock);
                queueSignal.wait_for (lock, std::chrono::milliseconds (100),
                                      [this] { return ! txQueue.empty() || threadShouldExit(); });
                if (txQueue.empty())
                    continue;
                frame = std::move (txQueue.front());
                txQueue.pop_front();
            }
            server.send (frame.data(), frame.size());
            std::this_thread::sleep_for (std::chrono::microseconds (frameGapUs));
        }
    }
}
