#pragma once

#include "instruments/InstrumentHub.h"
#include "instruments/InstrumentStyle.h"
#include "instruments/SignalAnalysis.h"

namespace kickemu
{
    /**
        Live list of MIDI messages in both directions, decoded: IN = into the KICK's
        MIDI IN (from the host), OUT = what the KICK sends on MIDI OUT. Recording is
        off until enabled (it is shared with the midi_monitor automation tool).
    */
    class MidiMonitorView : public juce::Component, private juce::ListBoxModel, private juce::Timer
    {
    public:
        MidiMonitorView (InstrumentHub& hub, const Theme& theme);
        ~MidiMonitorView() override;

        /** Pulls new messages from the hub (the timer does this 15 times a second). */
        void poll();
        void resized() override;
        void paint (juce::Graphics& g) override;

    private:
        struct Row
        {
            InstrumentHub::MidiEntry entry;
            signal::MidiText text;
        };

        int getNumRows() override { return (int) visible.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
        void timerCallback() override { poll(); }
        void rebuildVisible();
        void syncButtons();

        InstrumentHub& hub;
        const Theme& theme;
        std::vector<Row> rows;
        std::vector<int> visible;
        juce::uint64 cursor = 0;
        juce::uint64 countIn = 0, countOut = 0;

        juce::TextButton enableButton { "Enabled" }, pauseButton { "Pause" }, clearButton { "Clear" }, clockButton { "Hide clock" };
        juce::ListBox list;
    };
}
