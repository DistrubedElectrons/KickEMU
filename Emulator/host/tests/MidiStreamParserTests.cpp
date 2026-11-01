#include "emu/MidiStreamParser.h"

using namespace kickemu;

class MidiStreamParserTests : public juce::UnitTest
{
public:
    MidiStreamParserTests() : juce::UnitTest ("MIDI stream parser", "midi") {}

    std::vector<std::vector<juce::uint8>> parse (std::vector<juce::uint8> bytes, bool byteByByte = false)
    {
        MidiStreamParser p;
        std::vector<std::vector<juce::uint8>> out;
        p.onMessage = [&out] (const juce::uint8* d, size_t n) { out.emplace_back (d, d + n); };
        if (byteByByte)
            for (auto b : bytes)
                p.feed (&b, 1);
        else
            p.feed (bytes.data(), bytes.size());
        return out;
    }

    void runTest() override
    {
        using V = std::vector<juce::uint8>;

        beginTest ("Note on/off and running status");
        {
            auto m = parse ({ 0x90, 60, 100, 62, 90, 60, 0 });
            expectEquals ((int) m.size(), 3);
            expect (m[1] == V { 0x90, 62, 90 });
            expect (m[2] == V { 0x90, 60, 0 });
        }

        beginTest ("Program change (one data byte) with running status");
        {
            auto m = parse ({ 0xc3, 5, 6 });
            expectEquals ((int) m.size(), 2);
            expect (m[1] == V { 0xc3, 6 });
        }

        beginTest ("Real-time bytes inside a message and inside sysex");
        {
            auto m = parse ({ 0x90, 0xf8, 60, 0xfa, 100, 0xf0, 0x7e, 0xf8, 0x01, 0xf7 }, true);
            expectEquals ((int) m.size(), 5);
            expect (m[0] == V { 0xf8 });
            expect (m[1] == V { 0xfa });
            expect (m[2] == V { 0x90, 60, 100 });
            expect (m[3] == V { 0xf8 });
            expect (m[4] == V { 0xf0, 0x7e, 0x01, 0xf7 });
        }

        beginTest ("Long sysex split across chunks");
        {
            MidiStreamParser p;
            std::vector<V> out;
            p.onMessage = [&out] (const juce::uint8* d, size_t n) { out.emplace_back (d, d + n); };
            V head { 0xf0 };
            for (int i = 0; i < 1000; ++i) head.push_back ((juce::uint8) (i & 0x7f));
            p.feed (head.data(), head.size());
            juce::uint8 end = 0xf7;
            p.feed (&end, 1);
            expectEquals ((int) out.size(), 1);
            expectEquals ((int) out[0].size(), 1002);
        }

        beginTest ("System common cancels running status; stray data ignored");
        {
            auto m = parse ({ 0x40, 0xb0, 7, 100, 0xf2, 1, 2, 3 });
            expectEquals ((int) m.size(), 2);
            expect (m[1] == V { 0xf2, 1, 2 });
        }
    }
};

static MidiStreamParserTests midiStreamParserTests;
