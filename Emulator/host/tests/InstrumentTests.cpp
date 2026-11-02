#include "instruments/InstrumentHub.h"
#include "instruments/SignalAnalysis.h"

#include <cmath>

using namespace kickemu;

class InstrumentTests : public juce::UnitTest
{
public:
    InstrumentTests() : juce::UnitTest ("Instruments", "instruments") {}

    void runTest() override
    {
        constexpr double rate = 48000.0;
        auto tone = [&] (double hz, double amp, double dc, int n)
        {
            std::vector<float> x ((size_t) n);
            for (int i = 0; i < n; ++i)
                x[(size_t) i] = (float) (dc + amp * std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / rate + 0.3));
            return x;
        };

        beginTest ("multimeter: DC, RMS, peak, frequency of a sine with offset");
        {
            auto x = tone (1000.0, 0.5, 0.1, 12000);   // 250 ms
            auto m = signal::measure (x.data(), (int) x.size(), rate);
            expectWithinAbsoluteError (m.mean, 0.1, 1e-4);
            expectWithinAbsoluteError (m.acRms, 0.5 / std::sqrt (2.0), 1e-4);
            expectWithinAbsoluteError (m.rms, std::sqrt (0.01 + 0.125), 1e-4);
            expectWithinAbsoluteError (m.peak, 0.6, 1e-3);
            expectWithinAbsoluteError (m.frequency, 1000.0, 0.05);
            // 997 Hz over 25 ms: whole-sample crossing times would be off by ~1 Hz,
            // interpolated ones land within 0.05 Hz.
            x = tone (997.0, 0.25, 0.0, 1200);
            expectWithinAbsoluteError (signal::measure (x.data(), (int) x.size(), rate).frequency, 997.0, 0.05);
            std::vector<float> flat (100, 2.5f);
            m = signal::measure (flat.data(), 100, 1000.0);
            expectWithinAbsoluteError (m.mean, 2.5, 1e-6);
            expect (m.frequency == 0.0, "a constant has no frequency");
        }

        beginTest ("spectrum: a -6 dBFS sine peaks at its frequency and level");
        {
            auto x = tone (1234.5, 0.5, 0.0, 48000);
            auto s = signal::spectrum (x.data(), (int) x.size(), rate, 8192);
            auto peaks = signal::findPeaks (s, 3);
            expect (! peaks.empty());
            expectWithinAbsoluteError (peaks[0].frequency, 1234.5, 1.0);
            expectWithinAbsoluteError (peaks[0].db, 20.0 * std::log10 (0.5), 0.5);   // Hann scalloping < 1.5 dB, interpolated
            if (peaks.size() > 1)
                expect (peaks[1].db < -60.0, "no second tone: " + juce::String (peaks[1].db));
        }

        beginTest ("frame ring: newest frames across the wrap, mid of L/R");
        {
            FrameRing ring (2, 8, rate);
            std::vector<float> f;
            for (int i = 0; i < 11; ++i)
            {
                f.push_back ((float) i);
                f.push_back ((float) (100 + i));
            }
            ring.push (f.data(), 11);
            float out[8];
            juce::int64 end = 0;
            expectEquals (ring.read (0, 8, out, -1, end), 8);
            expectEquals ((int) end, 11);
            expectEquals (out[0], 3.0f);
            expectEquals (out[7], 10.0f);
            expectEquals (ring.read (-1, 2, out, 10, end), 2);
            expectEquals (out[1], 0.5f * (9.0f + 109.0f));
        }

        beginTest ("MIDI monitor: decode and split host bytes into messages");
        {
            const juce::uint8 noteOffAsOn[] = { 0x91, 60, 0 };
            auto t = signal::describeMidi (noteOffAsOn, 3);
            expectEquals (t.kind, juce::String ("note_off"));
            expectEquals (t.channel, 2);
            const juce::uint8 bend[] = { 0xe0, 0x00, 0x60 };
            expect (signal::describeMidi (bend, 3).text.contains ("4096"));

            Config config;
            config.loadFromString ("{}", juce::File());
            CvBridge cv (config);
            MidiBridge midi (config);
            InstrumentHub hub (config, cv, midi);
            midi.onMonitor (true, noteOffAsOn, 3);   // disabled: not recorded
            expectEquals ((int) hub.midiEnd(), 0);
            hub.setMidiMonitorEnabled (true);
            const juce::uint8 raw[] = { 0x90, 0x3c, 0x64, 0x3c, 0x00, 0xf8 };   // running status + clock
            midi.onMonitor (true, raw, sizeof (raw));
            const juce::uint8 cc[] = { 0xb0, 7, 100 };
            midi.onMonitor (false, cc, 3);
            auto e = hub.midiSince (0, 10);
            expectEquals ((int) e.size(), 4);
            if (e.size() == 4)
            {
                expect (e[0].toGuest && e[0].bytes.size() == 3);
                expectEquals (signal::describeMidi (e[1].bytes.data(), e[1].bytes.size()).kind, juce::String ("note_off"));
                expectEquals (signal::describeMidi (e[2].bytes.data(), e[2].bytes.size()).kind, juce::String ("clock"));
                expect (! e[3].toGuest);
            }
        }
    }
};

static InstrumentTests instrumentTests;
