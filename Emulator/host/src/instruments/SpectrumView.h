#pragma once

#include "instruments/InstrumentHub.h"
#include "instruments/InstrumentStyle.h"
#include "instruments/SignalAnalysis.h"

namespace kickemu
{
    /**
        FFT spectrum analyser: Hann window, dBFS (a full-scale sine reads 0 dB),
        logarithmic frequency axis, exponential power averaging and peak hold.
        Each new block of samples is analysed once, so a stopped source freezes
        the display instead of being averaged into itself.
    */
    class SpectrumView : public juce::Component, private juce::Timer
    {
    public:
        SpectrumView (InstrumentHub& hub, const Theme& theme);
        ~SpectrumView() override;

        struct Settings
        {
            juce::String source = "audio_mid";
            int fftSize = 8192;
            int average = 4;        // 1 = off
            bool peakHold = false;
            bool running = true;
        };
        Settings getSettings() const { return settings; }
        void setSettings (const Settings& s);

        void acquire();
        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        void timerCallback() override { if (isShowing()) acquire(); }
        void syncControls();
        juce::Rectangle<float> graphArea() const;
        float xOf (double hz, juce::Rectangle<float> r) const;
        double fMin() const;

        InstrumentHub& hub;
        const Theme& theme;
        Settings settings;

        std::vector<float> avgPower, holdDb;
        double rate = 48000.0;
        int analysedSize = 0;
        juce::int64 lastEnd = -1;
        int framesAveraged = 0;
        bool stale = true;
        std::vector<signal::Peak> peaks;

        juce::ComboBox sourceBox, sizeBox, avgBox;
        juce::TextButton holdButton { "Peak hold" }, runButton { "Run" };
        bool updating = false;
    };
}
