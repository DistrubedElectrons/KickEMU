#pragma once

#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        A backlit rubber key: rectangular keys with a printed legend that lights
        in the LED colour (dim grey when off), and the round transport buttons
        with a lit icon. SHIFT lights its outline.

        While SHIFT is held, keys with a shift function send the alternate id
        (e.g. MAIN -> GRID), as the KICK's front-panel firmware does.
    */
    class ButtonComponent : public PanelControl
    {
    public:
        ButtonComponent (PanelContext& context, const LayoutControl& definition);
        ~ButtonComponent() override;

        bool animate (double now, float dtMs) override;
        void setKeyDown (bool down) override;

        void paint (juce::Graphics& g) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        void rebuild() override;
        void press();
        void release();
        juce::Path iconPath (juce::Rectangle<float> area) const;
        juce::Colour ledColour (int colourId) const;

        struct LongPress : juce::Timer
        {
            std::function<void()> fire;
            void timerCallback() override { stopTimer(); if (fire) fire(); }
        } longPress;

        bool round = false, ring = false;
        juce::Path capPath;
        juce::Image capImage;
        paint::Mask legend, legendGlow, outline, outlineGlow, capGlow;
        float shownLevel = 0.0f, pressAmount = 0.0f;
        juce::Colour shownColour;
        bool down = false, keyHeld = false;
        int sentId = -1;
    };
}
