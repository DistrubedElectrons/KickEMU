#pragma once

#include "instruments/InstrumentHub.h"
#include "instruments/InstrumentStyle.h"
#include "instruments/SignalAnalysis.h"

namespace kickemu
{
    /**
        Two-channel oscilloscope on the instrument sources (audio tap, CV). Ten
        horizontal divisions, eight vertical; the trigger point sits at the centre.
        Audio is scaled in FS/div with 0 at the centre line, CV in V/div with 0 V on
        the bottom line. Channels from different rings (audio vs CV) are aligned by
        their arrival time on the host, so their relative timing is approximate.
    */
    class ScopeView : public juce::Component, private juce::Timer
    {
    public:
        ScopeView (InstrumentHub& hub, const Theme& theme);
        ~ScopeView() override;

        struct Channel
        {
            juce::String source;    // "" = off
            double perDiv = 0.25;   // FS or V per division
        };
        struct Settings
        {
            Channel ch[2] { { "audio_l", 0.25 }, { "cv1", 1.0 } };
            double secPerDiv = 0.001;
            int triggerChannel = 0;
            bool risingEdge = true;
            double triggerLevel = 0.0;   // in the trigger channel's unit
            bool autoMode = true;
            bool running = true;
        };
        Settings getSettings() const { return settings; }
        void setSettings (const Settings& s);

        /** Acquires a new screen now (the timer does this 30 times a second). */
        void acquire();

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        struct Trace
        {
            bool active = false, stale = false;
            InstrumentHub::Source source;
            std::vector<float> samples;
            double leftPos = 0.0;      // position of the left screen edge in samples
            double samplesPerDiv = 1.0;
            signal::Measurement m;
        };

        void timerCallback() override { if (isShowing()) acquire(); }
        void syncControls();
        void readControls();
        void fillSourceBox (juce::ComboBox& box);
        static void fillScaleBox (juce::ComboBox& box, bool audio, double current);
        juce::Rectangle<float> screenArea() const;
        void drawGrid (juce::Graphics& g, juce::Rectangle<float> r) const;
        void drawTrace (juce::Graphics& g, juce::Rectangle<float> r, const Trace& t, double perDiv, double zeroDiv, juce::Colour c) const;
        double zeroDivFor (const InstrumentHub::Source& s) const { return s.audio ? 0.0 : -4.0; }

        InstrumentHub& hub;
        const Theme& theme;
        Settings settings;
        Trace traces[2];
        juce::String status = "AUTO";

        juce::Label chLabel[2];
        juce::ComboBox sourceBox[2], scaleBox[2], timeBox, trigSourceBox;
        juce::TextButton edgeButton, modeButton, runButton { "Run" };
        juce::Slider levelSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label levelLabel { {}, "Level" };
        bool updating = false;
    };
}
