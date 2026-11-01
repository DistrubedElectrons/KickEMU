#pragma once

#include "config/Config.h"
#include "emu/ConsoleBridge.h"
#include "emu/CvBridge.h"
#include "emu/McuEmulator.h"
#include "emu/MidiBridge.h"
#include "emu/QemuCommandLine.h"
#include "emu/QemuProcess.h"
#include "emu/QmpClient.h"
#include "emu/RfbClient.h"
#include "panel/RearPanel.h"

#include <juce_events/juce_events.h>
#include <deque>

namespace kickemu
{
    /** Thread-safe log shared by all components; the log view polls it. */
    class LogBuffer
    {
    public:
        struct Entry
        {
            juce::Time time;
            juce::String source, text;
        };

        explicit LogBuffer (int maxLines) : limit (maxLines) {}

        void add (const juce::String& source, const juce::String& text);
        std::vector<Entry> since (juce::uint64& cursor) const;
        void clear();
        juce::uint64 getTotal() const { return total.load(); }

        bool echoToStdout = false;

    private:
        mutable std::mutex lock;
        std::deque<Entry> entries;
        std::atomic<juce::uint64> total { 0 };
        int limit;
    };

    /**
        Everything that makes up one emulator run: the UART/CV listening sockets,
        the MCU emulator, MIDI/CV/console bridges, QEMU itself and the QMP/RFB
        clients. The sockets live as long as the session, so QEMU can be
        restarted (or restart itself) without the host losing anything.
    */
    class Session : private juce::Timer
    {
    public:
        enum class QemuState { stopped, running, stopping };

        explicit Session (Config& config);
        ~Session() override;

        /** Creates the run directory and all listening sockets. */
        juce::Result startBridges();

        bool startQemu();
        void stopQemu();
        void restartQemu();
        QemuState getQemuState() const { return qemuState.load(); }

        Config& config;
        LogBuffer log;
        PanelState panel;
        McuEmulator mcu;
        MidiBridge midi;
        CvBridge cv;
        ConsoleBridge console, uart2;
        SharedFramebuffer framebuffer;
        QmpClient qmp;
        RfbClient rfb;
        IoStatus io;

        juce::String getLastCommandLine() const { return lastCommand; }
        juce::String getSdImage() const { return sdImage; }

        /** Called on the message thread when QEMU starts or stops. */
        std::function<void()> onStateChanged;

    private:
        void timerCallback() override;
        void setupIoStatus();
        void processExited (int code);

        QemuProcess qemu;
        std::atomic<QemuState> qemuState { QemuState::stopped };
        bool restartPending = false;
        std::atomic<bool> shuttingDown { false };
        juce::uint32 stopRequestedAt = 0;
        juce::String lastCommand, sdImage;
    };
}
