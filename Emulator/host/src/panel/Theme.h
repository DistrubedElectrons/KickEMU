#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickemu
{
    /**
        Colours and fonts of the device and the app chrome. Device colours come
        from the "theme" object in layout.json, the pad palette from the config
        (host.pad_palette, the PADBUTTON colours of Kick/assets/Default/theme.xml).
    */
    struct Theme
    {
        // Device
        juce::Colour enclosure { 0xff1e1f21 };
        juce::Colour enclosureEdge { 0xff2b2c2f };
        juce::Colour bevelLight { 0xff4a4b4f };
        juce::Colour recess { 0xff161719 };
        juce::Colour keycap { 0xff1b1c1e };
        juce::Colour keycapTop { 0xff27282b };
        juce::Colour labelPrint { 0xffd8d8d8 };      // white silkscreen (MASTER, GRID ...)
        juce::Colour labelOff { 0xff5c5d61 };        // unlit key legends
        juce::Colour wordmark { 0xffe6e6e6 };
        juce::Colour glass { 0xff050506 };
        juce::Colour padRubber { 0xff141416 };
        juce::Colour metal { 0xffb9bcc2 };

        // App chrome
        juce::Colour appBackground { 0xff121214 };
        juce::Colour chrome { 0xff19191c };
        juce::Colour chromeLine { 0xff2a2a2f };
        juce::Colour chromeText { 0xffc9cad0 };
        juce::Colour chromeDim { 0xff7b7d86 };
        juce::Colour accent { 0xff3fa9ff };
        juce::Colour ok { 0xff3ddc84 };
        juce::Colour warn { 0xffffb020 };
        juce::Colour bad { 0xffff4d4d };

        juce::String labelFontName = "DIN Alternate";
        juce::String keyFontName = "DIN Alternate";
        juce::String wordmarkFontName = "Avenir Next";
        juce::String headingFontName = "Avenir Next";
        juce::String uiFontName = "Helvetica Neue";
        juce::String monoFontName = "Menlo";

        juce::Array<juce::Colour> padPalette;

        /** Palette colour for a pad colour id (PadColourIds), or transparent. */
        juce::Colour padColour (int id) const;

        juce::Font labelFont (float capHeightPx) const;
        juce::Font keyFont (float capHeightPx) const;
        juce::Font wordmarkFont (float heightPx) const;
        juce::Font headingFont (float capHeightPx) const;
        juce::Font uiFont (float heightPx, bool bold = false) const;
        juce::Font monoFont (float heightPx) const;

        /** Loads overrides from a JSON object (layout.json "theme"). */
        void load (const juce::var& themeObject);

        static juce::Colour parseColour (const juce::var& v, juce::Colour fallback);
    };
}
