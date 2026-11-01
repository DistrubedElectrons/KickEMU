#pragma once

#include "app/AppSettings.h"
#include "app/Session.h"
#include "panel/Theme.h"

namespace kickemu
{
    /** MIDI device binding, CV inputs/outputs and control-surface status. */
    class IoView : public juce::Component, private juce::Timer
    {
    public:
        IoView (Session& session, AppSettings& settings, const Theme& theme);
        ~IoView() override;
        void paint (juce::Graphics& g) override;
        void resized() override;
        void refreshDevices();

    private:
        void timerCallback() override;

        Session& session;
        AppSettings& settings;
        const Theme& theme;

        juce::ComboBox midiIn, midiOut;
        juce::Array<juce::MidiDeviceInfo> inputs, outputs;
        juce::OwnedArray<juce::Slider> cvInputs;
        juce::OwnedArray<juce::Label> cvOutputs;
        juce::String mcuText;
        juce::Rectangle<int> mcuArea, cvHeader, midiHeader, mcuHeader;
    };

    /** App settings, keyboard map and version information. */
    class SettingsView : public juce::Component
    {
    public:
        SettingsView (Session& session, AppSettings& settings, const Theme& theme, juce::StringArray keyboardMap);
        void paint (juce::Graphics& g) override;
        void resized() override;

        std::function<void (juce::Colour)> onPresentationColour;

    private:
        Session& session;
        AppSettings& settings;
        const Theme& theme;
        juce::TextEditor presentationColour, info, keys;
        juce::Label presentationLabel;
        juce::TextButton revealSnapshots { "Open snapshot folder" };
    };
}
