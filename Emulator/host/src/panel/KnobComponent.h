#pragma once

#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        Rotary controls: the endless encoders P1-P4 (knurled skirt with a brushed
        metal cone and a white pointer) and the MASTER/REC pots (knurled, pointer,
        -150..+150 degrees).

        Encoders send KNOBS/EN_KNOB_ROTARY steps; drag vertically or use the
        scroll wheel. The pots are analog on the device (audio path, not the MCU),
        so they only keep their position.
    */
    class KnobComponent : public PanelControl
    {
    public:
        KnobComponent (PanelContext& context, const LayoutControl& definition);

        bool animate (double now, float dtMs) override;
        void paint (juce::Graphics& g) override;
        bool hitTest (int x, int y) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
        void mouseDoubleClick (const juce::MouseEvent& e) override;

        /** Pot position 0..1 (pots only); persisted by the app. */
        float getPotValue() const { return potValue; }
        void setPotValue (float v);
        std::function<void (float)> onPotChanged;

        /** Turns an encoder by steps (also used by the keyboard mapping). */
        void step (int steps);

        /** Draws a knob cap; shared with the jog wheel. */
        static void drawCap (juce::Graphics& g, juce::Rectangle<float> area, const Theme& theme, bool metalCone,
                             const juce::Image& noise, float pixelScale);

    private:
        void rebuild() override;
        float angle() const;

        bool encoder = false;
        juce::Image cap;            // cached cap without pointer
        float potValue = 0.5f;
        float encoderAngle = 0.0f, shownAngle = 0.0f;
        float dragAccumulator = 0.0f, lastDragY = 0.0f;
        float wheelAccumulator = 0.0f;
    };
}
