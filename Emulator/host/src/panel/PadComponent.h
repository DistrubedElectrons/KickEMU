#pragma once

#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        One of the 16 velocity pads: soft rubber face, RGB edge light that bleeds
        into the surroundings, and a slight press depression.

        Velocity comes from the click position (centre hits harder) or from the
        pressure of a force-sensitive trackpad/pen; while held, aftertouch follows
        pressure or vertical movement (only sent when the guest enabled it).
    */
    class PadComponent : public PanelControl
    {
    public:
        PadComponent (PanelContext& context, const LayoutControl& definition);

        bool animate (double now, float dtMs) override;
        void setKeyDown (bool down) override;

        void paint (juce::Graphics& g) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        void rebuild() override;
        void press (int velocity);
        void release();
        int velocityFor (const juce::MouseEvent& e) const;

        paint::Mask rim, bloom, hotEdge, innerTint;
        juce::Image face;
        juce::Path facePath;

        juce::Colour shownColour { juce::Colours::black }, targetColour { juce::Colours::black };
        float shownLevel = 0.0f, pressAmount = 0.0f;
        bool down = false, keyHeld = false;
        int lastVelocity = 0, lastAftertouch = -1;
        juce::uint32 lastAftertouchMs = 0;
        float pressY = 0.0f;
    };
}
