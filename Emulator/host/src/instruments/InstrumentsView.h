#pragma once

#include "instruments/MeterView.h"
#include "instruments/MidiMonitorView.h"
#include "instruments/ScopeView.h"
#include "instruments/SpectrumView.h"

namespace kickemu
{
    /**
        The measuring instruments in one component: oscilloscope, spectrum analyser,
        multimeter and MIDI monitor behind a segmented selector. Used as a side
        panel tab; "Window" opens a second, resizable copy in its own window.
    */
    class InstrumentsView : public juce::Component
    {
    public:
        enum Page { scope = 0, spectrum, meter, midi };

        InstrumentsView (InstrumentHub& hub, const Theme& theme, bool allowPopOut = true);
        ~InstrumentsView() override;

        void showPage (Page p);
        Page getPage() const { return page; }

        ScopeView& getScope() { return scopeView; }
        SpectrumView& getSpectrum() { return spectrumView; }
        MeterView& getMeter() { return meterView; }
        MidiMonitorView& getMidi() { return midiView; }

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        void openWindow();

        InstrumentHub& hub;
        const Theme& theme;
        Page page = scope;
        ScopeView scopeView;
        SpectrumView spectrumView;
        MeterView meterView;
        MidiMonitorView midiView;
        juce::OwnedArray<juce::TextButton> pageButtons;
        juce::TextButton windowButton { "Window" };
        std::unique_ptr<juce::DocumentWindow> window;
    };
}
