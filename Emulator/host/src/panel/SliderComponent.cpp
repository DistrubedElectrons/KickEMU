#include "panel/SliderComponent.h"

namespace kickemu
{
    SliderComponent::SliderComponent (PanelContext& context, const LayoutControl& definition)
        : PanelControl (context, definition, 2.0f)
    {
        count = juce::jlimit (1, PanelState::maxSliderLeds, (int) def.extra["led_count"]);
        ledColour = Theme::parseColour (def.extra["led_colour"], juce::Colours::red);
        shown.assign ((size_t) count, 0.0f);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    bool SliderComponent::hitTest (int x, int y) { return body.contains ((float) x, (float) y); }

    void SliderComponent::rebuild()
    {
        const auto offset = body.getTopLeft() - def.getBounds().getTopLeft() * pxPerMm;
        const float x = (float) (double) def.extra["led_x"] * pxPerMm + offset.x;
        const float top = (float) (double) def.extra["led_top"] * pxPerMm + offset.y;
        const float bottom = (float) (double) def.extra["led_bottom"] * pxPerMm + offset.y;
        juce::Point<float> size;
        if (auto* s = def.extra["led_size"].getArray(); s != nullptr && s->size() == 2)
            size = { (float) (double) (*s)[0] * pxPerMm, (float) (double) (*s)[1] * pxPerMm };

        // LED 0 is the bottom one.
        leds.clear();
        for (int i = 0; i < count; ++i)
        {
            const float cy = bottom - (bottom - top) * (float) i / (float) juce::jmax (1, count - 1);
            leds.push_back (juce::Rectangle<float> (size.x, size.y).withCentre ({ x, cy }));
        }

        juce::Path dot;
        dot.addRoundedRectangle (juce::Rectangle<float> (size.x, size.y), size.y * 0.25f);
        dotCore = paint::makeMask (dot, mm (0.12f), pixelScale);
        dotGlow = paint::makeMask (dot, mm (1.5f), pixelScale, mm (0.6f));

        // Touch strip: the part of the recess left of the LEDs.
        strip = body.withRight (x - size.x).reduced (mm (1.2f), mm (0.8f));
    }

    bool SliderComponent::animate (double, float dtMs)
    {
        bool changed = false;
        for (int i = 0; i < count; ++i)
        {
            const float old = shown[(size_t) i];
            shown[(size_t) i] = approach (old, ctx.state.sliderLed (i, count), dtMs, ctx.fadeMs);
            changed |= std::abs (old - shown[(size_t) i]) > 0.001f;
        }
        const float oldTouch = touchFade;
        touchFade = approach (touchFade, touching ? 1.0f : 0.0f, dtMs, 60.0f);
        return changed || std::abs (oldTouch - touchFade) > 0.001f;
    }

    void SliderComponent::paint (juce::Graphics& g)
    {
        // Unlit LED windows.
        for (auto& r : leds)
        {
            g.setColour (juce::Colour (0xff0a0a0b));
            g.fillRoundedRectangle (r, r.getHeight() * 0.25f);
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.drawRoundedRectangle (r, r.getHeight() * 0.25f, mm (0.08f));
        }

        for (int i = 0; i < count; ++i)
        {
            const float level = paint::ledAlpha (shown[(size_t) i]);
            if (level <= 0.0f)
                continue;
            auto r = leds[(size_t) i];
            juce::Graphics::ScopedSaveState s (g);
            g.addTransform (juce::AffineTransform::translation (r.getX(), r.getY()));
            dotGlow.draw (g, ledColour.withMultipliedAlpha (0.8f * level));
            dotCore.draw (g, ledColour.interpolatedWith (juce::Colours::white, 0.4f).withMultipliedAlpha (level));
        }

        // Finger feedback on the strip.
        if (touchFade > 0.0f)
        {
            auto spot = juce::Rectangle<float> (strip.getWidth(), mm (6.0f)).withCentre ({ strip.getCentreX(), touchY });
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f * touchFade), spot.getCentreX(), spot.getCentreY(),
                                                     juce::Colours::transparentWhite, spot.getCentreX(), spot.getY(), true));
            g.fillEllipse (spot);
        }
    }

    void SliderComponent::touchAt (float y)
    {
        touchY = juce::jlimit (strip.getY(), strip.getBottom(), y);
        const int percent = juce::roundToInt (100.0f * (strip.getBottom() - touchY) / juce::jmax (1.0f, strip.getHeight()));
        if (percent != lastPercent)
        {
            lastPercent = percent;
            ctx.mcu.sendSlider (percent);
        }
    }

    void SliderComponent::mouseDown (const juce::MouseEvent& e)
    {
        touching = true;
        lastPercent = -1;
        touchAt (e.position.y);
    }

    void SliderComponent::mouseDrag (const juce::MouseEvent& e) { touchAt (e.position.y); }
    void SliderComponent::mouseUp (const juce::MouseEvent&) { touching = false; }

    // ------------------------------------------------------------------------------------

    LedComponent::LedComponent (PanelContext& context, const LayoutControl& definition)
        : PanelControl (context, definition, 2.0f)
    {
    }

    void LedComponent::rebuild()
    {
        juce::Path p;
        p.addRoundedRectangle (body, mm (0.3f));
        core = paint::makeMask (p, mm (0.12f), pixelScale);
        glow = paint::makeMask (p, mm (1.6f), pixelScale, mm (0.8f));
    }

    bool LedComponent::animate (double now, float dtMs)
    {
        const float old = shown;
        shown = approach (shown, ctx.state.ledAppearance (def.led, now, ctx.blinkPeriodSeconds).brightness, dtMs, ctx.fadeMs);
        return std::abs (old - shown) > 0.001f;
    }

    void LedComponent::paint (juce::Graphics& g)
    {
        g.setColour (juce::Colour (0xff09090a));
        g.fillRoundedRectangle (body, mm (0.3f));
        const float level = paint::ledAlpha (shown);
        if (level > 0.0f)
        {
            glow.draw (g, def.colour.withMultipliedAlpha (0.8f * level));
            core.draw (g, def.colour.interpolatedWith (juce::Colours::white, 0.35f).withMultipliedAlpha (level));
        }
    }
}
