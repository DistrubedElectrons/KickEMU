#pragma once

#include "emu/ConsoleBridge.h"
#include "panel/Theme.h"

namespace kickemu
{
    /**
        Renders a TerminalModel and sends typed keys to the console UART
        (xterm key sequences for arrows, Home/End, Delete, Ctrl+letter).
    */
    class TerminalView : public juce::Component, private juce::Timer
    {
    public:
        TerminalView (ConsoleBridge& console, const Theme& theme, float fontSize);

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;
        void mouseDown (const juce::MouseEvent&) override { grabKeyboardFocus(); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
        void focusGained (FocusChangeType) override { repaint(); }
        void focusLost (FocusChangeType) override { repaint(); }

    private:
        void timerCallback() override;
        juce::Colour colourFor (int index, juce::uint32 rgb, bool bold) const;

        ConsoleBridge& console;
        const Theme& theme;
        juce::Font font;
        float cellW = 7.0f, cellH = 14.0f;
        juce::uint32 lastRevision = 0;
        int scrollOffset = 0;   // lines scrolled back
        bool lastConnected = false;
    };
}
