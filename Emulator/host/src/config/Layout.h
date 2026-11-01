#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

namespace kickemu
{
    /** One interactive control on the top view. All geometry is in millimetres
        from the top-left corner of the enclosure. */
    struct LayoutControl
    {
        juce::String id;
        juce::String type;             // pad, button, round_button, jog, jog_key, pot, encoder, slider, led, screen
        juce::Rectangle<float> rect;   // for rectangular controls
        juce::Point<float> centre;     // for round controls
        float radius = 0.0f;
        juce::String label;            // text printed on the key cap (may contain \n)
        juce::String icon;             // transport / jog key icon name
        juce::String key;              // keyboard shortcut (JUCE key description)
        juce::String tip;              // tooltip
        juce::Colour colour;           // LED colour
        int index = -1;                // pad index 0..15
        int hw = -1;                   // BtnLedSequence id sent when pressed
        int shiftHw = -1;              // id sent while SHIFT is held
        int led = -1;                  // LED id driving the light of this control
        int knob = -1;                 // KnobSequence id for encoders and the jog wheel
        int pushHw = -1;               // jog push button id
        juce::Point<int> quadrant;     // jog keys: (-1|1, -1|1)
        juce::var extra;               // remaining properties (slider LEDs, jog ring, ...)

        juce::Rectangle<float> getBounds() const
        {
            if (! rect.isEmpty())
                return rect;
            return { centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f };
        }
    };

    /** A printed text on a panel. */
    struct LayoutText
    {
        juce::String text;
        juce::Point<float> position;   // anchor (centre or left edge, vertically centred)
        float size = 3.0f;             // cap height in mm
        juce::String colour = "label";
        juce::String align = "centre";
        juce::String font = "label";
    };

    /** A non-interactive decoration (recesses, bezel, lines, dots). */
    struct LayoutDecor
    {
        juce::String type;
        juce::Rectangle<float> rect;
        juce::Point<float> a, b;       // line endpoints, or centre in a
        float radius = 0.0f;
        juce::var extra;
    };

    /** Connector on the rear or front strip. */
    struct LayoutConnector
    {
        juce::String id, type;
        juce::Point<float> centre;
        juce::Rectangle<float> rect;
        juce::Point<float> size;
        float radius = 0.0f;
        int channel = -1;
    };

    struct LayoutStrip
    {
        float width = 0.0f, height = 0.0f;
        juce::Rectangle<float> panel;
        juce::Array<juce::Point<float>> feet; // x, width
        float footHeight = 0.0f;
        juce::Array<float> dividers;
        float dividerTop = 0.0f, dividerBottom = 0.0f;
        juce::Array<juce::Point<float>> screws;
        float screwRadius = 0.0f;
        juce::Array<LayoutConnector> connectors;
        juce::Array<LayoutText> texts;
        LayoutText wordmark;
    };

    /** Device geometry loaded from resources/layout.json. */
    class Layout
    {
    public:
        juce::Result parse (const juce::String& json);

        float width = 270.0f, height = 270.0f, cornerRadius = 3.0f, bevel = 2.0f;
        std::vector<LayoutControl> controls;
        std::vector<LayoutText> texts;
        std::vector<LayoutDecor> decor;
        LayoutStrip rear, front;

        const LayoutControl* find (const juce::String& id) const;
        std::vector<const LayoutControl*> ofType (const juce::String& type) const;
    };
}
