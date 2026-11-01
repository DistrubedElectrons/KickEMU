#pragma once

#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        The touch slider left of the pads, with its LED column. Dragging sends
        SLIDER/EN_SLIDER_TOUCH_INPUT in percent (bottom 0, top 100); the LEDs show
        what the guest set with the BAR/LEVEL/INDIVIDUAL LED commands.
    */
    class SliderComponent : public PanelControl
    {
    public:
        SliderComponent (PanelContext& context, const LayoutControl& definition);

        bool animate (double now, float dtMs) override;
        void paint (juce::Graphics& g) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        void rebuild() override;
        void touchAt (float y);

        int count = 16;
        juce::Colour ledColour;
        std::vector<juce::Rectangle<float>> leds;
        std::vector<float> shown;
        paint::Mask dotCore, dotGlow;      // one LED at the origin, translated per LED
        juce::Rectangle<float> strip;
        int lastPercent = -1;
        bool touching = false;
        float touchY = 0.0f, touchFade = 0.0f;
    };

    /** A single indicator LED (the four LEDs above the LINK key). */
    class LedComponent : public PanelControl
    {
    public:
        LedComponent (PanelContext& context, const LayoutControl& definition);

        bool animate (double now, float dtMs) override;
        void paint (juce::Graphics& g) override;
        bool hitTest (int, int) override { return false; }

    private:
        void rebuild() override;
        paint::Mask core, glow;
        float shown = 0.0f;
    };
}
