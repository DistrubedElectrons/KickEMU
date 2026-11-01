#include "panel/ButtonComponent.h"

#include <contracts/dcl/dcl_contracts.h>

namespace kickemu
{
    using DC = contracts::DeviceControlContracts;

    ButtonComponent::ButtonComponent (PanelContext& context, const LayoutControl& definition)
        : PanelControl (context, definition, 3.0f)
    {
        round = def.type == "round_button";
        ring = def.extra["style"].toString() == "ring";
        longPress.fire = [this]
        {
            if (down && sentId >= 0)
                ctx.mcu.sendButton (sentId, DC::BTN_LONGPRESSED);
        };
    }

    ButtonComponent::~ButtonComponent() { longPress.stopTimer(); }

    bool ButtonComponent::hitTest (int x, int y) { return body.expanded (mm (0.4f)).contains ((float) x, (float) y); }

    juce::Path ButtonComponent::iconPath (juce::Rectangle<float> a) const
    {
        juce::Path p;
        const auto icon = def.icon;
        const float s = a.getWidth();
        auto c = a.getCentre();

        if (icon == "record")
        {
            p.addEllipse (juce::Rectangle<float> (s * 0.62f, s * 0.62f).withCentre (c));
        }
        else if (icon == "stop")
        {
            p.addRoundedRectangle (juce::Rectangle<float> (s * 0.58f, s * 0.58f).withCentre (c), s * 0.07f);
        }
        else if (icon == "play")
        {
            const float h = s * 0.62f;
            p.addTriangle (c.x - h * 0.38f, c.y - h * 0.5f, c.x - h * 0.38f, c.y + h * 0.5f, c.x + h * 0.52f, c.y);
            p = p.createPathWithRoundedCorners (s * 0.05f);
        }
        else if (icon == "playpause")
        {
            const float h = s * 0.58f;
            const float x0 = c.x - h * 0.62f;
            juce::Path tri;
            tri.addTriangle (x0, c.y - h * 0.5f, x0, c.y + h * 0.5f, x0 + h * 0.72f, c.y);
            p.addPath (tri.createPathWithRoundedCorners (s * 0.04f));
            p.addRoundedRectangle (x0 + h * 0.82f, c.y - h * 0.5f, h * 0.2f, h, h * 0.04f);
            p.addRoundedRectangle (x0 + h * 1.12f, c.y - h * 0.5f, h * 0.2f, h, h * 0.04f);
        }
        else if (icon == "mic")
        {
            // Capsule, cradle, stem and foot.
            const float h = s * 0.66f;
            const float top = c.y - h * 0.5f;
            p.addRoundedRectangle (c.x - h * 0.17f, top, h * 0.34f, h * 0.56f, h * 0.17f);
            juce::Path cradle;
            cradle.startNewSubPath (c.x - h * 0.29f, top + h * 0.32f);
            cradle.cubicTo (c.x - h * 0.29f, top + h * 0.78f, c.x + h * 0.29f, top + h * 0.78f, c.x + h * 0.29f, top + h * 0.32f);
            juce::PathStrokeType (h * 0.08f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (cradle, cradle);
            p.addPath (cradle);
            p.addRectangle (c.x - h * 0.04f, top + h * 0.72f, h * 0.08f, h * 0.18f);
            p.addRoundedRectangle (c.x - h * 0.2f, top + h * 0.88f, h * 0.4f, h * 0.09f, h * 0.04f);
        }
        return p;
    }

    void ButtonComponent::rebuild()
    {
        auto& t = ctx.theme;
        const float radius = round ? body.getWidth() * 0.5f : mm (1.1f);
        capPath.clear();
        if (round)
            capPath.addEllipse (body);
        else
            capPath.addRoundedRectangle (body, radius);

        // Key cap: soft rubber, slightly lighter at the top, fine grain, dark edge.
        const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);
        capImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (capImage);
            g.addTransform (juce::AffineTransform::scale (pixelScale));
            g.setGradientFill (juce::ColourGradient (t.keycapTop, body.getX(), body.getY(),
                                                     t.keycap.darker (0.2f), body.getX(), body.getBottom(), false));
            g.fillPath (capPath);
            paint::fillNoise (g, capPath, ctx.noise, 0.05f, pixelScale);

            if (round)
            {
                // Convex dome highlight.
                g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), body.getCentreX(), body.getY() + body.getHeight() * 0.2f,
                                                         juce::Colours::transparentWhite, body.getCentreX(), body.getCentreY() + body.getHeight() * 0.2f, true));
                g.fillEllipse (body.reduced (body.getWidth() * 0.12f));
            }

            // Top bevel highlight and a dark outline.
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            juce::Path topEdge;
            topEdge.addRoundedRectangle (body.reduced (mm (0.15f)), radius);
            g.strokePath (topEdge, juce::PathStrokeType (mm (0.12f)), juce::AffineTransform::translation (0.0f, mm (0.05f)));
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.strokePath (capPath, juce::PathStrokeType (mm (0.15f)));
        }

        // Legend / icon masks: crisp for the print, blurred for the glow.
        if (round)
        {
            auto icon = iconPath (body.reduced (body.getWidth() * 0.18f));
            legend = paint::makeMask (icon, 0.0f, pixelScale);
            legendGlow = paint::makeMask (icon, mm (0.9f), pixelScale);
            juce::Path rim;
            rim.addEllipse (body.reduced (mm (0.35f)));
            capGlow = paint::makeMask (rim, mm (0.8f), pixelScale, mm (0.5f));
        }
        else
        {
            const bool twoLines = def.label.containsChar ('\n');
            auto font = t.keyFont (mm (twoLines ? 2.05f : 2.3f));
            auto area = body.reduced (mm (0.4f));
            legend = paint::makeTextMask (def.label, font, area, juce::Justification::centred, 1.0f, 0.0f, pixelScale);
            legendGlow = paint::makeTextMask (def.label, font, area, juce::Justification::centred, 1.0f, mm (0.7f), pixelScale);
        }

        if (ring)
        {
            juce::Path edge;
            edge.addRoundedRectangle (body.reduced (mm (0.45f)), radius * 0.8f);
            outline = paint::makeMask (edge, mm (0.1f), pixelScale, mm (0.35f));
            outlineGlow = paint::makeMask (edge, mm (1.2f), pixelScale, mm (0.8f));
        }
    }

    juce::Colour ButtonComponent::ledColour (int colourId) const
    {
        if (ctx.buttonLedFromPalette && colourId > 0)
        {
            auto c = ctx.theme.padColour (colourId);
            if (! c.isTransparent())
                return c;
        }
        return def.colour;
    }

    bool ButtonComponent::animate (double now, float dtMs)
    {
        // A key shows its own LED, and the LED of its shift function (bank E lights A, GRID lights MAIN).
        float target = 0.0f;
        int colourId = 0;
        for (int id : { def.led, def.shiftHw })
        {
            if (id < 0)
                continue;
            auto a = ctx.state.ledAppearance (id, now, ctx.blinkPeriodSeconds);
            if (a.brightness > target)
            {
                target = a.brightness;
                colourId = a.colourId;
            }
        }

        const auto oldLevel = shownLevel, oldPress = pressAmount;
        const auto oldColour = shownColour;
        shownColour = ledColour (colourId);
        shownLevel = approach (shownLevel, target, dtMs, ctx.fadeMs);
        pressAmount = approach (pressAmount, down ? 1.0f : 0.0f, dtMs, down ? 10.0f : 40.0f);
        return std::abs (oldLevel - shownLevel) > 0.001f || std::abs (oldPress - pressAmount) > 0.001f || oldColour != shownColour;
    }

    void ButtonComponent::paint (juce::Graphics& g)
    {
        const float level = paint::ledAlpha (shownLevel);
        const auto c = shownColour;

        {
            juce::Graphics::ScopedSaveState s (g);
            const float k = 1.0f - 0.03f * pressAmount;
            auto centre = body.getCentre();
            g.addTransform (juce::AffineTransform::scale (k, k, centre.x, centre.y).translated (0.0f, mm (0.1f) * pressAmount));
            g.drawImageTransformed (capImage, juce::AffineTransform::scale (1.0f / pixelScale));
            if (pressAmount > 0.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (0.22f * pressAmount));
                g.fillPath (capPath);
            }

            // Legend: dim grey when off, LED colour with bloom when lit.
            auto off = round ? ctx.theme.labelOff.brighter (0.35f) : ctx.theme.labelOff;
            legend.draw (g, off.withMultipliedAlpha (1.0f - level));
            if (level > 0.0f)
            {
                legendGlow.draw (g, c.withMultipliedAlpha (0.85f * level));
                legend.draw (g, c.interpolatedWith (juce::Colours::white, 0.35f).withMultipliedAlpha (level));
                if (round)
                    capGlow.draw (g, c.withMultipliedAlpha (0.28f * level));
                if (ring)
                {
                    outlineGlow.draw (g, c.withMultipliedAlpha (0.7f * level));
                    outline.draw (g, c.interpolatedWith (juce::Colours::white, 0.3f).withMultipliedAlpha (level));
                }
            }
        }
    }

    void ButtonComponent::press()
    {
        down = true;
        if (def.hw == DC::EN_SHIFT)
            ctx.shiftHeld = true;

        sentId = (ctx.shiftAlternates && ctx.shiftHeld.load() && def.shiftHw >= 0) ? def.shiftHw : def.hw;
        ctx.mcu.sendButton (sentId, DC::BTN_PRESSED);
        if (ctx.longPressMs > 0)
            longPress.startTimer (ctx.longPressMs);
        if (ctx.onHint)
            ctx.onHint (def.tip + " (id " + juce::String (sentId) + ")");
    }

    void ButtonComponent::release()
    {
        if (! down)
            return;
        down = false;
        longPress.stopTimer();
        if (def.hw == DC::EN_SHIFT)
            ctx.shiftHeld = false;
        ctx.mcu.sendButton (sentId, DC::BTN_RELEASED);
    }

    void ButtonComponent::mouseDown (const juce::MouseEvent&) { press(); }
    void ButtonComponent::mouseUp (const juce::MouseEvent&) { release(); }

    void ButtonComponent::setKeyDown (bool isDown)
    {
        if (isDown == keyHeld)
            return;
        keyHeld = isDown;
        if (isDown)
            press();
        else
            release();
    }
}
