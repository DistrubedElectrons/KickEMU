#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace kickemu
{
    /**
        Turns a raw MIDI byte stream (as on a DIN cable / UART) into complete
        messages: handles running status, real-time bytes interleaved anywhere,
        and system exclusive of any length.
    */
    class MidiStreamParser
    {
    public:
        /** Called once per complete message (status byte included). */
        std::function<void (const juce::uint8* data, size_t size)> onMessage;

        void feed (const juce::uint8* data, size_t size);
        void reset();

        /** Number of data bytes that follow a status byte (-1 for sysex). */
        static int dataBytesFor (juce::uint8 status);

    private:
        juce::uint8 runningStatus = 0;
        std::vector<juce::uint8> current;
        int expected = 0;
        bool inSysex = false;
    };
}
