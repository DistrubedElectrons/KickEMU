#pragma once

#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        The jog wheel block: four keys around it (undo, copy, minus, plus), the
        red light-guide ring with its LEDs, and the push encoder in the middle.

        Turning sends KNOBS/EN_KNOB_ROTARY on EN_KNOB_CENTER; a click without
        turning is the push (EN_KNOB_ENTER). The ring follows LED ids from
        layout.json when the guest drives them, otherwise it is lit while the
        MCU link is up (host.mcu.jog_ring_default).
    */
    class JogWheelComponent : public PanelControl
    {
    public:
        JogWheelComponent (PanelContext& context, const LayoutControl& wheel, std::vector<const LayoutControl*> keys);

        bool animate (double now, float dtMs) override;
        void paint (juce::Graphics& g) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

        /** Keyboard: turn, push and the four keys. */
        void turn (int steps);
        void setPushDown (bool down);
        void setKeyDown (int keyIndex, bool down);
        using PanelControl::setKeyDown;

    private:
        void rebuild() override;
        int quadrantAt (juce::Point<float> p) const;
        float ringTarget (int ledIndex, double now) const;

        std::vector<const LayoutControl*> keyDefs;   // undo, copy, minus, plus (by quadrant)
        juce::Rectangle<float> housing;
        juce::Point<float> centre;
        float knobR = 0, ringR = 0, ringW = 0, recessR = 0;

        juce::Image housingImage, skirtImage, coneImage;
        std::array<juce::Path, 4> keyPaths;
        paint::Mask ringGlow, ringCore;
        std::vector<paint::Mask> hotspots;
        std::vector<int> ringLeds;
        juce::Colour ringColour;

        std::vector<float> shownRing;
        std::array<float, 4> keyPress {};
        std::array<bool, 4> keyDown {};
        float wheelAngle = 0.0f, shownWheelAngle = 0.0f, pushAmount = 0.0f;

        enum class Drag { none, knob, key } drag = Drag::none;
        int dragKey = -1;
        float lastAngle = 0.0f, angleAccumulator = 0.0f, wheelAccumulator = 0.0f;
        bool turned = false, pushSent = false, pushHeld = false;
        juce::uint32 downTime = 0;
    };
}
