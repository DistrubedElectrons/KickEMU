#pragma once

#include "panel/Theme.h"

namespace kickemu
{
    /** Small flat button showing a vector icon. */
    class IconButton : public juce::Button
    {
    public:
        IconButton (const juce::String& name, const Theme& theme, std::function<juce::Path (juce::Rectangle<float>)> icon);
        void setIcon (std::function<juce::Path (juce::Rectangle<float>)> icon) { makeIcon = std::move (icon); repaint(); }
        void setHighlighted (bool on) { highlighted = on; repaint(); }
        void paintButton (juce::Graphics& g, bool over, bool down) override;

    private:
        const Theme& theme;
        std::function<juce::Path (juce::Rectangle<float>)> makeIcon;
        bool highlighted = false;
    };

    /** Segmented control (Top / Rear / Front). */
    class Segmented : public juce::Component
    {
    public:
        Segmented (const Theme& theme, juce::StringArray items);
        void setSelected (int index);
        int getSelected() const { return selected; }
        std::function<void (int)> onChange;

        void paint (juce::Graphics& g) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        const Theme& theme;
        juce::StringArray items;
        int selected = 0;
    };

    /**
        The slim bar above the device: name and run state on the left, view
        switch in the middle, scale and actions on the right.
    */
    class TitleBar : public juce::Component
    {
    public:
        explicit TitleBar (const Theme& theme);

        struct Status
        {
            juce::String qemu;          // Running / Stopped / Stopping
            juce::Colour qemuColour;
            juce::String detail;        // machine, profile
            bool mcuLinked = false, uartConnected = false, displayConnected = false;
        };
        void setStatus (const Status& s);

        Segmented views;
        juce::ComboBox scale;
        IconButton power, restart, snapshot, presentation, panel;

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        const Theme& theme;
        Status status;
        juce::Rectangle<int> statusArea;
    };

    namespace icons
    {
        juce::Path play (juce::Rectangle<float>);
        juce::Path stop (juce::Rectangle<float>);
        juce::Path restart (juce::Rectangle<float>);
        juce::Path camera (juce::Rectangle<float>);
        juce::Path expand (juce::Rectangle<float>);
        juce::Path sidebar (juce::Rectangle<float>);
    }
}
