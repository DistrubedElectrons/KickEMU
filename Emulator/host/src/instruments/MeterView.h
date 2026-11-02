#pragma once

#include "instruments/InstrumentHub.h"
#include "instruments/InstrumentStyle.h"
#include "instruments/SignalAnalysis.h"

namespace kickemu
{
    /**
        Two-input multimeter. Each input reads one source over a window (100 ms,
        250 ms or 1 s) and shows a large primary reading (RMS in dBFS for audio,
        DC volts for CV), the secondary readings (DC, RMS, peak, min/max,
        frequency) and a bar graph.
    */
    class MeterView : public juce::Component, private juce::Timer
    {
    public:
        MeterView (InstrumentHub& hub, const Theme& theme);
        ~MeterView() override;

        struct Input
        {
            juce::String source;
            double windowSec = 0.25;
        };
        void setInput (int index, const Input& input);
        Input getInput (int index) const { return inputs[index]; }

        void acquire();
        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        void timerCallback() override { if (isShowing()) acquire(); }
        void sync (int index);
        juce::Rectangle<float> panelArea (int index) const;
        void paintPanel (juce::Graphics& g, int index, juce::Rectangle<float> r) const;

        InstrumentHub& hub;
        const Theme& theme;
        Input inputs[2] { { "audio_l", 0.25 }, { "cv1", 0.25 } };
        signal::Measurement readings[2];
        bool stale[2] { true, true };
        InstrumentHub::Source sources[2];
        juce::ComboBox sourceBox[2], windowBox[2];
        bool updating = false;
    };
}
