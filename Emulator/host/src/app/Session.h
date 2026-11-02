#pragma once

#include "automation/ConsoleCapture.h"
#include "app/Peripherals.h"
#include "config/Config.h"
#include "emu/ConsoleBridge.h"
#include "emu/CvBridge.h"
#include "emu/McuEmulator.h"
#include "emu/MidiBridge.h"
#include "emu/QemuCommandLine.h"
#include "emu/QemuProcess.h"
#include "emu/QmpClient.h"
#include "emu/RfbClient.h"
#include "instruments/InstrumentHub.h"
#include "panel/RearPanel.h"

#include <juce_events/juce_events.h>
#include <cstdio>
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

        /** Mirror every entry to this stream (stdout in headless mode, stderr under --mcp). */
        std::FILE* echo = nullptr;

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
        /** Scope/spectrum/multimeter sources (audio tap, CV) and the MIDI monitor. */
        InstrumentHub instruments { config, cv, midi };
        SharedFramebuffer framebuffer;
        QmpClient qmp;
        RfbClient rfb;
        IoStatus io;
        Peripherals peripherals { config, qmp };

        /** PWM1 duty (panel backlight, 0..1000), refreshed by pollBacklight(). */
        std::atomic<int> backlightPermille { 1000 };
        /** Asks QEMU for the backlight duty; no-op when it is not running. */
        void pollBacklight();

        /** Console text for automation (escape sequences removed, absolute offsets). */
        ConsoleCapture consoleText;

        /** MIDI messages the guest sent on DIN OUT, numbered from 0. */
        struct MidiEvent
        {
            juce::uint64 index;
            double time;                    // seconds since the session started
            std::vector<juce::uint8> bytes;
        };
        std::vector<MidiEvent> midiSince (juce::uint64 index, size_t max) const;
        juce::uint64 midiEnd() const;

        /** Raw console output (stdout mirror in headless mode), on the socket thread. */
        std::function<void (const juce::uint8*, size_t)> onConsoleOutput;

        /** Milliseconds since QEMU was last started (0 when stopped). */
        juce::int64 getUptimeMs() const;

        juce::String getLastCommandLine() const { return lastCommand; }

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
        juce::String lastCommand;
        std::atomic<juce::int64> qemuStartedAt { 0 };
        const double sessionStart = juce::Time::getMillisecondCounterHiRes();

        mutable std::mutex midiLock;
        std::deque<MidiEvent> midiLog;
        juce::uint64 midiTotal = 0;
    };
}
