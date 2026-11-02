#pragma once

#include "panel/Theme.h"

namespace kickemu::instr
{
    /** Shared look of the instrument screens: a dark glass with a phosphor grid. */
    struct Style
    {
        juce::Colour screen { 0xff08090b };
        juce::Colour screenEdge { 0xff26272c };
        juce::Colour gridMinor { 0xff15181b };
        juce::Colour grid { 0xff20262a };
        juce::Colour axis { 0xff36404a };
        juce::Colour label { 0xff6f7782 };
        juce::Colour readout { 0xffd9dde3 };
        juce::Colour ch1 { 0xfff2c94c };       // amber
        juce::Colour ch2 { 0xff4fd1c5 };       // teal
        juce::Colour trig { 0xffff7a59 };
        juce::Colour spectrum { 0xff6cc6ff };
        juce::Colour hold { 0xffff7a59 };
    };

    inline const Style& style()
    {
        static const Style s;
        return s;
    }

    /** Glass panel with a hairline edge; returns the inner area. */
    inline juce::Rectangle<float> drawScreen (juce::Graphics& g, juce::Rectangle<float> r)
    {
        auto& s = style();
        g.setColour (s.screen);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (s.screenEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        return r.reduced (1.0f);
    }

    /** A trace drawn twice: a soft wide glow, then the crisp line. */
    inline void strokeTrace (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.4f)
    {
        g.setColour (c.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (width + 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    /** Engineering formatting: 1.23 kHz, 12.0 ms, 3.30 V. */
    inline juce::String eng (double v, const juce::String& unit, int digits = 3)
    {
        if (! std::isfinite (v))
            return "--- " + unit;
        const double a = std::abs (v);
        if (a < 1.0e-12)
            return "0 " + unit;
        struct P { double f; const char* p; };
        static const P prefixes[] = { { 1e9, "G" }, { 1e6, "M" }, { 1e3, "k" }, { 1.0, "" }, { 1e-3, "m" }, { 1e-6, "u" }, { 1e-9, "n" } };
        for (auto& p : prefixes)
            if (a >= p.f * 0.9995 || p.f < 1e-8)
            {
                const double x = v / p.f;
                const int intDigits = std::abs (x) >= 100.0 ? 3 : std::abs (x) >= 10.0 ? 2 : 1;
                const int decimals = juce::jmax (0, digits - intDigits);
                // juce::String (double, 0) would print every digit
                const auto number = decimals > 0 ? juce::String (x, decimals) : juce::String (juce::roundToInt (x));
                return number + " " + p.p + unit;
            }
        return juce::String (v) + " " + unit;
    }

    /** Small button that shows its state by colour (Run/Stop, Auto/Normal ...). */
    inline void styleToggle (juce::TextButton& b, juce::Colour on)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on.withAlpha (0.28f));
        b.setColour (juce::TextButton::textColourOnId, on.brighter (0.4f));
    }
}
