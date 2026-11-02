#pragma once

#include "config/Config.h"
#include "emu/MidiStreamParser.h"
#include "emu/UartServer.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace kickemu
{
    /**
        UART3 (DIN MIDI) <-> host MIDI.

        Bytes from the guest are split into messages (running status, sysex,
        real-time) and sent to a virtual source ("KICK MIDI Out") and optionally
        an existing output device. Messages from the virtual destination
        ("KICK MIDI In") and optionally an existing input device are written to
        the guest as raw bytes.
    */
    class MidiBridge : private juce::MidiInputCallback
    {
    public:
        explicit MidiBridge (const Config& config);
        ~MidiBridge() override;

        bool start (juce::String& error);
        void stop();

        /** Binds an existing host device in addition to the virtual ports (empty = none). */
        void setOutputDevice (const juce::String& identifier);
        void setInputDevice (const juce::String& identifier);
        juce::String getOutputDevice() const { return outputId; }
        juce::String getInputDevice() const { return inputId; }

        bool hasVirtualPorts() const { return virtualOut != nullptr && virtualIn != nullptr; }
        juce::String getVirtualOutName() const { return outName; }
        juce::String getVirtualInName() const { return inName; }
        bool isGuestConnected() const { return server.isConnected(); }

        /** Messages guest -> host (DIN OUT) and host -> guest (DIN IN). */
        std::atomic<juce::uint64> messagesFromGuest { 0 }, messagesToGuest { 0 };

        std::function<void (const juce::String&)> onLog;

        /** Every complete message the guest sent (DIN OUT), on the socket thread. */
        std::function<void (const juce::uint8*, size_t)> onGuestMessage;

        /** Writes raw MIDI bytes to the guest's DIN IN. Returns false if QEMU is not attached. */
        bool sendToGuest (const juce::uint8* data, size_t size);

    private:
        void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;
        void toHost (const juce::uint8* data, size_t size);

        const Config& config;
        UartServer server { "midi" };
        MidiStreamParser parser;
        juce::String outName, inName, outputId, inputId;

        std::mutex outputLock;
        std::unique_ptr<juce::MidiOutput> virtualOut, deviceOut;
        std::unique_ptr<juce::MidiInput> virtualIn, deviceIn;
    };
}
