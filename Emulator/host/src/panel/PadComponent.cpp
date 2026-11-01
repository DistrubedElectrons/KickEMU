#include "panel/PadComponent.h"

#include <contracts/dcl/dcl_contracts.h>

namespace kickemu
{
    using DC = contracts::DeviceControlContracts;

    PadComponent::PadComponent (PanelContext& context, const LayoutControl& definition)
        : PanelControl (context, definition, 4.5f)
    {
    }

    bool PadComponent::hitTest (int x, int y) { return body.contains ((float) x, (float) y); }

    void PadComponent::rebuild()
    {
        const float r = mm (1.1f);
        facePath.clear();
        facePath.addRoundedRectangle (body, r);

        // Rubber face: near black, lit from the top, with a fine grain.
        const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);
        face = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (face);
            g.addTransform (juce::AffineTransform::scale (pixelScale));
            auto& t = ctx.theme;
            g.setGradientFill (juce::ColourGradient (t.padRubber.brighter (0.12f), body.getX(), body.getY(),
                                                     t.padRubber.darker (0.25f), body.getX(), body.getBottom(), false));
            g.fillPath (facePath);
            paint::fillNoise (g, facePath, ctx.noise, 0.05f, pixelScale);

            // Soft top highlight and a darker lower edge give the slightly domed rubber look.
            juce::Path top;
            top.addRoundedRectangle (body.reduced (mm (0.5f)).withHeight (body.getHeight() * 0.45f), r);
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.045f), body.getX(), body.getY(),
                                                     juce::Colours::transparentWhite, body.getX(), body.getY() + body.getHeight() * 0.45f, false));
            g.fillPath (top);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.strokePath (facePath, juce::PathStrokeType (mm (0.18f)));
        }

        juce::Path ring;
        ring.addRoundedRectangle (body.expanded (mm (0.25f)), r + mm (0.25f));
        rim = paint::makeMask (ring, mm (0.22f), pixelScale, mm (0.45f));
        bloom = paint::makeMask (ring, mm (2.6f), pixelScale, mm (1.6f));

        // The light pipe is brightest along the top and right edges (as on the device).
        juce::Path hot;
        auto e = body.expanded (mm (0.3f));
        hot.startNewSubPath (e.getX() + r, e.getY());
        hot.lineTo (e.getRight() - r, e.getY());
        hot.quadraticTo (e.getRight(), e.getY(), e.getRight(), e.getY() + r);
        hot.lineTo (e.getRight(), e.getBottom() - r);
        hotEdge = paint::makeMask (hot, mm (0.35f), pixelScale, mm (0.75f));

        juce::Path inner;
        inner.addRoundedRectangle (body.reduced (mm (0.2f)), r);
        innerTint = paint::makeMask (inner, mm (1.8f), pixelScale, mm (1.4f));
    }

    bool PadComponent::animate (double now, float dtMs)
    {
        auto a = ctx.state.padAppearance (def.index, now, ctx.blinkPeriodSeconds);
        const float targetLevel = a.colourId >= 0 ? a.brightness : 0.0f;
        if (a.colourId >= 0)
            targetColour = ctx.theme.padColour (a.colourId);

        const auto oldLevel = shownLevel, oldPress = pressAmount;
        const auto oldColour = shownColour;

        shownLevel = approach (shownLevel, targetLevel, dtMs, ctx.fadeMs);
        shownColour = shownLevel <= 0.0f && targetLevel <= 0.0f ? targetColour
                                                                 : shownColour.interpolatedWith (targetColour, juce::jmin (1.0f, dtMs / juce::jmax (1.0f, ctx.fadeMs)));
        pressAmount = approach (pressAmount, down ? 1.0f : 0.0f, dtMs, down ? 12.0f : 45.0f);

        return std::abs (oldLevel - shownLevel) > 0.001f || std::abs (oldPress - pressAmount) > 0.001f
               || (shownLevel > 0.0f && oldColour != shownColour);
    }

    void PadComponent::paint (juce::Graphics& g)
    {
        const float level = paint::ledAlpha (shownLevel);
        const auto c = shownColour;

        if (level > 0.0f)
            bloom.draw (g, c.withMultipliedAlpha (0.55f * level));

        // Face, pushed in slightly while pressed.
        {
            juce::Graphics::ScopedSaveState s (g);
            const float k = 1.0f - 0.022f * pressAmount;
            auto centre = body.getCentre();
            g.addTransform (juce::AffineTransform::scale (k, k, centre.x, centre.y).translated (0.0f, mm (0.12f) * pressAmount));
            g.drawImageTransformed (face, juce::AffineTransform::scale (1.0f / pixelScale));
            if (pressAmount > 0.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (0.18f * pressAmount));
                g.fillPath (facePath);
            }
            if (level > 0.0f)
            {
                g.reduceClipRegion (facePath);
                innerTint.draw (g, c.withMultipliedAlpha (0.16f * level));
            }
        }

        if (level > 0.0f)
        {
            rim.draw (g, c.withMultipliedAlpha (0.75f * level));
            hotEdge.draw (g, c.interpolatedWith (juce::Colours::white, 0.25f).withMultipliedAlpha (level));
        }
    }

    int PadComponent::velocityFor (const juce::MouseEvent& e) const
    {
        const int lo = ctx.velocityMin, hi = ctx.velocityMax;
        if (e.isPressureValid() && e.pressure > 0.0f)
            return juce::jlimit (1, 127, lo + juce::roundToInt ((float) (hi - lo) * e.pressure));

        // Distance from the centre: centre = hardest hit, edges = softest.
        auto c = body.getCentre();
        auto d = juce::jmax (std::abs (e.position.x - c.x) / (body.getWidth() * 0.5f),
                             std::abs (e.position.y - c.y) / (body.getHeight() * 0.5f));
        return juce::jlimit (1, 127, juce::roundToInt ((float) hi - (float) (hi - lo) * juce::jlimit (0.0f, 1.0f, d)));
    }

    void PadComponent::press (int velocity)
    {
        down = true;
        lastVelocity = velocity;
        lastAftertouch = -1;
        ctx.mcu.sendPad (def.hw, DC::BTN_PRESSED, velocity, 0);
    }

    void PadComponent::release()
    {
        if (! down)
            return;
        down = false;
        ctx.mcu.sendPad (def.hw, DC::BTN_RELEASED, 0, 0);
    }

    void PadComponent::mouseDown (const juce::MouseEvent& e)
    {
        pressY = e.position.y;
        press (velocityFor (e));
    }

    void PadComponent::mouseDrag (const juce::MouseEvent& e)
    {
        if (! down || ! ctx.state.aftertouchEnabled.load())
            return;

        // Pressure if the input device has it, otherwise how far the finger moved up.
        int pressure;
        if (e.isPressureValid())
            pressure = juce::roundToInt (127.0f * e.pressure);
        else
            pressure = juce::jlimit (0, 127, juce::roundToInt ((pressY - e.position.y) / body.getHeight() * 254.0f));

        auto now = juce::Time::getMillisecondCounter();
        if (pressure != lastAftertouch && now - lastAftertouchMs >= (juce::uint32) ctx.aftertouchIntervalMs)
        {
            lastAftertouch = pressure;
            lastAftertouchMs = now;
            ctx.mcu.sendPadAftertouch (def.hw, lastVelocity, pressure);
        }
    }

    void PadComponent::mouseUp (const juce::MouseEvent&) { release(); }

    void PadComponent::setKeyDown (bool isDown)
    {
        if (isDown == keyHeld)
            return;
        keyHeld = isDown;
        if (isDown)
            press (ctx.velocityMax);
        else
            release();
    }
}
