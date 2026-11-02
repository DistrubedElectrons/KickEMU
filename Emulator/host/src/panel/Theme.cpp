#include "panel/Theme.h"

#include <KickEmuResources.h>
#include <map>
#include <mutex>

namespace kickemu
{
    namespace
    {
        // Condensed face shipped with the app (Barlow Semi Condensed, OFL), for hosts
        // without the configured DIN face: generic sans faces are too wide for the keys.
        const juce::String embeddedCondensed = "@barlow-semi-condensed";

        enum class Role { condensed, display, ui, mono };

        /** The first installed face of the configured name and a per-role fallback list.
            JUCE does not substitute an unknown face on Linux: it renders nothing. */
        juce::String installed (const juce::String& wanted, Role role)
        {
            static std::mutex lock;
            static std::map<juce::String, juce::String> cache;
            static const juce::StringArray all = juce::Font::findAllTypefaceNames();

            std::lock_guard<std::mutex> guard (lock);
            const auto key = wanted + "/" + juce::String ((int) role);
            if (auto it = cache.find (key); it != cache.end())
                return it->second;

            juce::StringArray candidates { wanted };
            switch (role)
            {
                case Role::condensed: candidates.addArray ({ "DIN Alternate", "Barlow Semi Condensed", "Roboto Condensed", "DejaVu Sans Condensed", "Liberation Sans Narrow", "Arial Narrow" }); break;
                case Role::display: candidates.addArray ({ "Avenir Next", "Montserrat", "Cantarell", "Noto Sans", "DejaVu Sans" }); break;
                case Role::ui: candidates.addArray ({ "Helvetica Neue", "Inter", "Cantarell", "Noto Sans", "Ubuntu", "DejaVu Sans", "Liberation Sans" }); break;
                case Role::mono: candidates.addArray ({ "Menlo", "JetBrains Mono", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono" }); break;
            }
            // Every role ends on the common sans faces: a wider face beats no text.
            if (role != Role::mono)
                candidates.addArray ({ "Noto Sans", "DejaVu Sans", "Liberation Sans", "Arial", "Helvetica" });
            if (role == Role::condensed && ! all.contains (wanted, true))
            {
                cache[key] = embeddedCondensed;
                return embeddedCondensed;
            }
            juce::String found = role == Role::mono ? juce::Font::getDefaultMonospacedFontName() : juce::Font::getDefaultSansSerifFontName();
            for (auto& c : candidates)
                if (all.contains (c, true))
                {
                    found = c;
                    break;
                }
            cache[key] = found;
            return found;
        }

        // Fonts are sized by cap height so printed text matches the measured
        // label heights in layout.json regardless of the font's metrics.

        juce::Typeface::Ptr embeddedCondensedTypeface()
        {
            static auto tf = juce::Typeface::createSystemTypefaceFor (KickEmuResources::BarlowSemiCondensedSemiBold_ttf,
                                                                      (size_t) KickEmuResources::BarlowSemiCondensedSemiBold_ttfSize);
            return tf;
        }

        juce::Font fontWithCapHeight (const juce::String& name, int styleFlags, float capHeightPx, float horizontalScale = 1.0f)
        {
            juce::Font f = name == embeddedCondensed ? juce::Font (juce::FontOptions (embeddedCondensedTypeface()).withHeight (100.0f))
                                                     : juce::Font (juce::FontOptions (name, 100.0f, styleFlags));
            juce::GlyphArrangement ga;
            ga.addLineOfText (f, "H", 0, 0);
            // Glyph bounds in JUCE 8 are line boxes; measure the outline instead.
            juce::Path outline;
            ga.createPath (outline);
            auto capAt100 = outline.getBounds().getHeight();
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

    juce::Font Theme::labelFont (float capHeightPx) const { return fontWithCapHeight (installed (labelFontName, Role::condensed), juce::Font::bold, capHeightPx); }
    juce::Font Theme::keyFont (float capHeightPx) const { return fontWithCapHeight (installed (keyFontName, Role::condensed), juce::Font::bold, capHeightPx, 0.92f); }
    juce::Font Theme::wordmarkFont (float heightPx) const
    {
        juce::Font font (juce::FontOptions (installed (wordmarkFontName, Role::display), heightPx, juce::Font::plain).withStyle ("Medium"));
        return font;
    }
    juce::Font Theme::headingFont (float capHeightPx) const { return fontWithCapHeight (installed (headingFontName, Role::display), juce::Font::bold, capHeightPx); }
    juce::Font Theme::uiFont (float heightPx, bool bold) const { return juce::Font (juce::FontOptions (installed (uiFontName, Role::ui), heightPx, bold ? juce::Font::bold : juce::Font::plain)); }
    juce::Font Theme::monoFont (float heightPx) const { return juce::Font (juce::FontOptions (installed (monoFontName, Role::mono), heightPx, juce::Font::plain)); }
}
