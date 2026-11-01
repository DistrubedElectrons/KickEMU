#include "panel/Painting.h"

namespace kickemu::paint
{
    void Mask::draw (juce::Graphics& g, juce::Colour colour) const
    {
        if (! image.isValid() || colour.getAlpha() == 0)
            return;
        g.setColour (colour);
        auto t = juce::AffineTransform::scale (bounds.getWidth() / (float) image.getWidth(),
                                               bounds.getHeight() / (float) image.getHeight())
                     .translated (bounds.getX(), bounds.getY());
        g.drawImageTransformed (image, t, true);
    }

    void blurSingleChannel (juce::Image& image, int radius)
    {
        if (radius < 1 || ! image.isValid())
            return;

        const int w = image.getWidth(), h = image.getHeight();
        juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);
        std::vector<int> line ((size_t) juce::jmax (w, h));
        std::vector<int> out ((size_t) juce::jmax (w, h));

        auto boxPass = [&] (int n)
        {
            // Sliding-window box filter with clamped edges.
            const int window = radius * 2 + 1;
            int sum = 0;
            for (int i = -radius; i <= radius; ++i)
                sum += line[(size_t) juce::jlimit (0, n - 1, i)];
            for (int i = 0; i < n; ++i)
            {
                out[(size_t) i] = sum / window;
                sum += line[(size_t) juce::jmin (n - 1, i + radius + 1)] - line[(size_t) juce::jmax (0, i - radius)];
            }
        };

        for (int pass = 0; pass < 3; ++pass)
        {
            for (int y = 0; y < h; ++y)
            {
                auto* row = data.getLinePointer (y);
                for (int x = 0; x < w; ++x)
                    line[(size_t) x] = row[x * data.pixelStride];
                boxPass (w);
                for (int x = 0; x < w; ++x)
                    row[x * data.pixelStride] = (juce::uint8) out[(size_t) x];
            }
            for (int x = 0; x < w; ++x)
            {
                for (int y = 0; y < h; ++y)
                    line[(size_t) y] = data.getLinePointer (y)[x * data.pixelStride];
                boxPass (h);
                for (int y = 0; y < h; ++y)
                    data.getLinePointer (y)[x * data.pixelStride] = (juce::uint8) out[(size_t) y];
            }
        }
    }

    Mask makeMask (const juce::Path& path, float blurRadius, float pixelScale, float strokeWidth)
    {
        Mask m;
        auto area = path.getBounds().expanded (blurRadius * 2.0f + strokeWidth + 1.0f);
        const int w = juce::jmax (1, juce::roundToInt (area.getWidth() * pixelScale));
        const int h = juce::jmax (1, juce::roundToInt (area.getHeight() * pixelScale));
        m.image = juce::Image (juce::Image::SingleChannel, w, h, true, juce::SoftwareImageType());
        {
            juce::Graphics g (m.image);
            g.addTransform (juce::AffineTransform::translation (-area.getX(), -area.getY()).scaled (pixelScale));
            g.setColour (juce::Colours::white);
            if (strokeWidth > 0.0f)
                g.strokePath (path, juce::PathStrokeType (strokeWidth));
            else
                g.fillPath (path);
        }
        blurSingleChannel (m.image, juce::roundToInt (blurRadius * pixelScale / 1.7f));
        m.bounds = area;
        return m;
    }

    void drawLines (juce::Graphics& g, const juce::String& text, const juce::Font& font,
                    juce::Rectangle<float> area, juce::Justification just, float lineSpacing)
    {
        auto lines = juce::StringArray::fromTokens (text, "\n", {});
        g.setFont (font);
        const float lineHeight = font.getHeight() * lineSpacing;
        const float total = lineHeight * (float) lines.size();
        float y = area.getCentreY() - total * 0.5f;
        for (auto& l : lines)
        {
            g.drawText (l, juce::Rectangle<float> (area.getX(), y, area.getWidth(), lineHeight), just, false);
            y += lineHeight;
        }
    }

    Mask makeTextMask (const juce::String& text, const juce::Font& font, juce::Rectangle<float> area,
                       juce::Justification just, float lineSpacing, float blurRadius, float pixelScale)
    {
        Mask m;
        auto bounds = area.expanded (blurRadius * 2.0f + 1.0f);
        const int w = juce::jmax (1, juce::roundToInt (bounds.getWidth() * pixelScale));
        const int h = juce::jmax (1, juce::roundToInt (bounds.getHeight() * pixelScale));
        m.image = juce::Image (juce::Image::SingleChannel, w, h, true, juce::SoftwareImageType());
        {
            // Scale font and area directly instead of using a transform: glyphs drawn
            // into a scaled software context come out at the unscaled size.
            juce::Graphics g (m.image);
            g.setColour (juce::Colours::white);
            auto scaledArea = (area - bounds.getPosition()) * pixelScale;
            drawLines (g, text, font.withHeight (font.getHeight() * pixelScale), scaledArea, just, lineSpacing);
        }
        blurSingleChannel (m.image, juce::roundToInt (blurRadius * pixelScale / 1.7f));
        m.bounds = bounds;
        return m;
    }

    juce::Image noiseTile (int size, juce::uint32 seed)
    {
        juce::Image img (juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
        juce::Random rng ((juce::int64) seed);
        juce::Image::BitmapData data (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                // Mix of fine grain and slightly larger speckles, centred on mid grey.
                auto v = (juce::uint8) juce::jlimit (0, 255, 128 + (int) ((rng.nextFloat() - 0.5f) * 255.0f));
                data.setPixelColour (x, y, juce::Colour (v, v, v));
            }
        }
        return img;
    }

    void fillNoise (juce::Graphics& g, const juce::Path& area, const juce::Image& tile, float opacity, float pixelScale)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (area);
        g.setOpacity (opacity);
        g.setTiledImageFill (tile, 0, 0, opacity);
        juce::ignoreUnused (pixelScale);
        g.fillPath (area);
    }

    void dropShadow (juce::Graphics& g, const juce::Path& path, juce::Point<float> offset, float radius, float opacity)
    {
        juce::DropShadow ds (juce::Colours::black.withAlpha (opacity), juce::jmax (1, juce::roundToInt (radius)),
                             { juce::roundToInt (offset.x), juce::roundToInt (offset.y) });
        ds.drawForPath (g, path);
    }

    void innerShadow (juce::Graphics& g, const juce::Path& path, float depth, float opacity)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (path);
        juce::Path outside;
        outside.addRectangle (path.getBounds().expanded (depth * 4.0f));
        outside.addPath (path);
        outside.setUsingNonZeroWinding (false);
        juce::DropShadow ds (juce::Colours::black.withAlpha (opacity), juce::jmax (1, juce::roundToInt (depth * 2.0f)),
                             { juce::roundToInt (depth * 0.6f), juce::roundToInt (depth) });
        ds.drawForPath (g, outside);
    }
}
