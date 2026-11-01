#include "panel/Theme.h"

namespace kickemu
{
    namespace
    {
        // Fonts are sized by cap height so printed text matches the measured
        // label heights in layout.json regardless of the font's metrics.
        juce::Font fontWithCapHeight (const juce::String& name, int styleFlags, float capHeightPx, float horizontalScale = 1.0f)
        {
            juce::Font f (juce::FontOptions (name, 100.0f, styleFlags));
            juce::GlyphArrangement ga;
            ga.addLineOfText (f, "H", 0, 0);
            auto capAt100 = ga.getBoundingBox (0, 1, true).getHeight();
            auto height = capAt100 > 0.0f ? 100.0f * capHeightPx / capAt100 : capHeightPx * 1.4f;
            return f.withHeight (height).withHorizontalScale (horizontalScale);
        }
    }

    juce::Colour Theme::parseColour (const juce::var& v, juce::Colour fallback)
    {
        auto s = v.toString().trim().trimCharactersAtStart ("#");
        if (s.isEmpty())
            return fallback;
        if (s.length() <= 6)
            return juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) s.getHexValue32()));
        return juce::Colour ((juce::uint32) s.getHexValue32());
    }

    void Theme::load (const juce::var& t)
    {
        if (! t.isObject())
            return;
        auto c = [&t] (const char* key, juce::Colour& target) { target = parseColour (t[key], target); };
        c ("enclosure", enclosure);
        c ("enclosure_edge", enclosureEdge);
        c ("bevel_light", bevelLight);
        c ("recess", recess);
        c ("keycap", keycap);
        c ("keycap_top", keycapTop);
        c ("label_print", labelPrint);
        c ("label_off", labelOff);
        c ("wordmark", wordmark);
        c ("glass", glass);
        c ("pad_rubber", padRubber);
        c ("metal", metal);

        auto f = [&t] (const char* key, juce::String& target) { if (t.hasProperty (key)) target = t[key].toString(); };
        f ("label_font", labelFontName);
        f ("key_font", keyFontName);
        f ("wordmark_font", wordmarkFontName);
        f ("heading_font", headingFontName);
    }

    juce::Colour Theme::padColour (int id) const
    {
        if (! juce::isPositiveAndBelow (id, padPalette.size()))
            return juce::Colours::transparentBlack;
        return padPalette[id];
    }

    juce::Font Theme::labelFont (float capHeightPx) const { return fontWithCapHeight (labelFontName, juce::Font::bold, capHeightPx); }
    juce::Font Theme::keyFont (float capHeightPx) const { return fontWithCapHeight (keyFontName, juce::Font::bold, capHeightPx, 0.92f); }
    juce::Font Theme::wordmarkFont (float heightPx) const
    {
        juce::Font font (juce::FontOptions (wordmarkFontName, heightPx, juce::Font::plain).withStyle ("Medium"));
        return font;
    }
    juce::Font Theme::headingFont (float capHeightPx) const { return fontWithCapHeight (headingFontName, juce::Font::bold, capHeightPx); }
    juce::Font Theme::uiFont (float heightPx, bool bold) const { return juce::Font (juce::FontOptions (uiFontName, heightPx, bold ? juce::Font::bold : juce::Font::plain)); }
    juce::Font Theme::monoFont (float heightPx) const { return juce::Font (juce::FontOptions (monoFontName, heightPx, juce::Font::plain)); }
}
