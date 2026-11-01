#include "emu/MidiStreamParser.h"

namespace kickemu
{
    int MidiStreamParser::dataBytesFor (juce::uint8 status)
    {
        if (status < 0xf0)
        {
            switch (status & 0xf0)
            {
                case 0xc0: case 0xd0: return 1;   // program change, channel pressure
                default: return 2;                // note on/off, poly AT, CC, pitch bend
            }
        }
        switch (status)
        {
            case 0xf0: return -1;                 // sysex: until 0xf7
            case 0xf1: case 0xf3: return 1;       // MTC quarter frame, song select
            case 0xf2: return 2;                  // song position
            default: return 0;                    // tune request, real-time
        }
    }

    void MidiStreamParser::reset()
    {
        runningStatus = 0;
        current.clear();
        expected = 0;
        inSysex = false;
    }

    void MidiStreamParser::feed (const juce::uint8* data, size_t size)
    {
        auto emit = [this] (const juce::uint8* d, size_t n) { if (onMessage) onMessage (d, n); };

        for (size_t i = 0; i < size; ++i)
        {
            const auto b = data[i];

            // Real-time messages may appear anywhere, even inside sysex.
            if (b >= 0xf8)
            {
                emit (&b, 1);
                continue;
            }

            if (inSysex)
            {
                current.push_back (b);
                if (b == 0xf7)
                {
                    emit (current.data(), current.size());
                    current.clear();
                    inSysex = false;
                }
                else if (b & 0x80)
                {
                    // A status byte terminates an unfinished sysex; drop it and re-parse.
                    current.clear();
                    inSysex = false;
                    --i;
                }
                continue;
            }

            if (b & 0x80)
            {
                current.assign (1, b);
                if (b == 0xf0)
                {
                    inSysex = true;
                    runningStatus = 0;
                    continue;
                }
                expected = dataBytesFor (b);
                // System common messages cancel running status.
                runningStatus = b < 0xf0 ? b : 0;
                if (expected == 0)
                {
                    emit (current.data(), current.size());
                    current.clear();
                }
                continue;
            }

            // Data byte.
            if (current.empty())
            {
                if (runningStatus == 0)
                    continue; // stray data byte
                current.assign (1, runningStatus);
                expected = dataBytesFor (runningStatus);
            }
            current.push_back (b);
            if ((int) current.size() - 1 == expected)
            {
                emit (current.data(), current.size());
                current.clear();
            }
        }
    }
}
