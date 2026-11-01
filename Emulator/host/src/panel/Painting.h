#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace kickemu::paint
{
    /**
        A single-channel mask rendered at device pixel resolution, positioned in
        component coordinates. Drawn tinted with any colour, which makes LED
        glows cheap: the expensive blur happens once per scale change.
    */
    struct Mask
    {
        juce::Image image;                  // SingleChannel
        juce::Rectangle<float> bounds;      // where it goes, in component (logical) pixels

        bool isValid() const { return image.isValid(); }

        /** Draws the mask filled with colour (alpha multiplies). */
        void draw (juce::Graphics& g, juce::Colour colour) const;
    };

    /** Renders a filled path into a mask with optional blur (radius in logical px).
        `pixelScale` is logical-to-physical (display scale factor). */
    Mask makeMask (const juce::Path& path, float blurRadius, float pixelScale, float strokeWidth = 0.0f);

    /** Renders text into a mask (crisp when blurRadius is 0). */
    Mask makeTextMask (const juce::String& text, const juce::Font& font, juce::Rectangle<float> area,
                       juce::Justification just, float lineSpacing, float blurRadius, float pixelScale);

    /** In-place blur of a single-channel image: three box passes (close to Gaussian). */
    void blurSingleChannel (juce::Image& image, int radiusPx);

    /** Tileable monochrome noise used for the matte enclosure and pad rubber. */
    juce::Image noiseTile (int size, juce::uint32 seed);

    /** Fills a path with tiled noise at the given opacity (adds texture). */
    void fillNoise (juce::Graphics& g, const juce::Path& area, const juce::Image& tile, float opacity, float pixelScale);

    /** Soft drop shadow for a path (approximated with stacked expanding fills). */
    void dropShadow (juce::Graphics& g, const juce::Path& path, juce::Point<float> offset, float radius, float opacity);

    /** Inner shadow along the top/left edges of a recess. */
    void innerShadow (juce::Graphics& g, const juce::Path& path, float depth, float opacity);

    /** Multi-line text drawn centred, lines separated by '\n'. */
    void drawLines (juce::Graphics& g, const juce::String& text, const juce::Font& font,
                    juce::Rectangle<float> area, juce::Justification just, float lineSpacing);

    /** Brightness to alpha with a little gamma so LED fades look linear to the eye. */
    inline float ledAlpha (float brightness) { return juce::jlimit (0.0f, 1.0f, std::pow (brightness, 0.8f)); }
}
