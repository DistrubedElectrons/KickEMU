#include "panel/KnobComponent.h"

namespace kickemu
{
    namespace
    {
        constexpr float potRangeDegrees = 150.0f;
        constexpr float encoderDegreesPerStep = 15.0f;
        const float lightAngle = juce::degreesToRadians (-135.0f); // light from the top left
    }

    KnobComponent::KnobComponent (PanelContext& context, const LayoutControl& definition)
        : PanelControl (context, definition, 2.5f)
    {
        encoder = def.type == "encoder";
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    bool KnobComponent::hitTest (int x, int y)
    {
        return body.getCentre().getDistanceFrom ({ (float) x, (float) y }) <= body.getWidth() * 0.55f;
    }

    void KnobComponent::drawCap (juce::Graphics& g, juce::Rectangle<float> area, const Theme& theme, bool metalCone,
                                 const juce::Image& noise, float physicalScale)
    {
        const auto c = area.getCentre();
        const float r = area.getWidth() * 0.5f;

        // Soft shadow cast to the bottom right.
        {
            juce::Path s;
            s.addEllipse (area.translated (r * 0.10f, r * 0.16f));
            paint::dropShadow (g, s, { 0.0f, 0.0f }, r * 0.35f, 0.75f);
        }

        // Knurled skirt: dark body with ridges whose brightness follows the light.
        juce::Path skirt;
        skirt.addEllipse (area);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a2b2e), c.x - r * 0.6f, c.y - r * 0.7f,
                                                 juce::Colour (0xff0b0b0c), c.x + r * 0.7f, c.y + r * 0.8f, false));
        g.fillPath (skirt);

        const int ridges = 40;
        for (int i = 0; i < ridges; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) ridges;
            const float lit = 0.5f + 0.5f * std::cos (a - lightAngle);
            juce::Path ridge;
            const float half = juce::MathConstants<float>::pi / (float) ridges * 0.45f;
            ridge.startNewSubPath (c.x + r * 0.80f * std::cos (a - half), c.y + r * 0.80f * std::sin (a - half));
            ridge.lineTo (c.x + r * 0.995f * std::cos (a - half), c.y + r * 0.995f * std::sin (a - half));
            ridge.lineTo (c.x + r * 0.995f * std::cos (a + half), c.y + r * 0.995f * std::sin (a + half));
            ridge.lineTo (c.x + r * 0.80f * std::cos (a + half), c.y + r * 0.80f * std::sin (a + half));
            ridge.closeSubPath();
            g.setColour (juce::Colour::greyLevel (0.08f + 0.22f * lit * lit));
            g.fillPath (ridge);
        }
        paint::fillNoise (g, skirt, noise, 0.04f, physicalScale);

        // Top face.
        const float topR = r * 0.80f;
        auto top = juce::Rectangle<float> (topR * 2.0f, topR * 2.0f).withCentre (c);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26272a), c.x - topR, c.y - topR,
                                                 juce::Colour (0xff101112), c.x + topR, c.y + topR, false));
        g.fillEllipse (top);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (top, r * 0.03f);

        if (metalCone)
        {
            // Brushed metal cone: the radial brushing makes two bright lobes facing
            // the light, which stay put while the knob turns.
            const float coneR = r * 0.66f;
            const int wedges = 96;
            for (int i = 0; i < wedges; ++i)
            {
                const float a0 = juce::MathConstants<float>::twoPi * (float) i / (float) wedges;
                const float a1 = juce::MathConstants<float>::twoPi * (float) (i + 1) / (float) wedges + 0.01f;
                const float am = (a0 + a1) * 0.5f;
                const float lobe = std::pow (0.5f + 0.5f * std::cos (2.0f * (am - lightAngle)), 3.0f);
                const float side = 0.5f + 0.5f * std::cos (am - lightAngle);
                const float v = 0.10f + 0.62f * lobe * (0.45f + 0.55f * side);
                juce::Path w;
                w.startNewSubPath (c);
                w.lineTo (c.x + coneR * std::cos (a0), c.y + coneR * std::sin (a0));
                w.lineTo (c.x + coneR * std::cos (a1), c.y + coneR * std::sin (a1));
                w.closeSubPath();
                g.setColour (juce::Colour::greyLevel (v).withMultipliedSaturation (0.0f));
                g.fillPath (w);
            }
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawEllipse (juce::Rectangle<float> (coneR * 2.0f, coneR * 2.0f).withCentre (c), r * 0.025f);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawEllipse (juce::Rectangle<float> (coneR * 2.0f, coneR * 2.0f).withCentre (c).translated (-r * 0.01f, -r * 0.015f), r * 0.015f);
        }
        juce::ignoreUnused (theme);
    }

    void KnobComponent::rebuild()
    {
        const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);
        cap = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        juce::Graphics g (cap);
        g.addTransform (juce::AffineTransform::scale (pixelScale));
        drawCap (g, body, ctx.theme, encoder, ctx.noise, pixelScale);
    }

    float KnobComponent::angle() const
    {
        if (encoder)
            return juce::degreesToRadians (shownAngle);
        return juce::degreesToRadians (-potRangeDegrees + 2.0f * potRangeDegrees * potValue);
    }

    bool KnobComponent::animate (double, float dtMs)
    {
        if (! encoder)
            return false;
        const float old = shownAngle;
        shownAngle = approach (shownAngle, encoderAngle, dtMs, 25.0f);
        return std::abs (old - shownAngle) > 0.01f;
    }

    void KnobComponent::paint (juce::Graphics& g)
    {
        g.drawImageTransformed (cap, juce::AffineTransform::scale (1.0f / pixelScale));

        // White pointer line (rotates); 0 degrees points up.
        const auto c = body.getCentre();
        const float r = body.getWidth() * 0.5f;
        const float a = angle() - juce::MathConstants<float>::halfPi;
        const float from = encoder ? r * 0.12f : r * 0.30f, to = encoder ? r * 0.64f : r * 0.86f;
        juce::Line<float> pointer (c.x + from * std::cos (a), c.y + from * std::sin (a), c.x + to * std::cos (a), c.y + to * std::sin (a));
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawLine (pointer.withShortenedStart (-r * 0.01f), r * 0.11f);
        g.setColour (juce::Colour (0xfff2f2f2));
        g.drawLine (pointer, r * 0.065f);
    }

    void KnobComponent::setPotValue (float v)
    {
        potValue = juce::jlimit (0.0f, 1.0f, v);
        repaint();
    }

    void KnobComponent::step (int steps)
    {
        if (steps == 0)
            return;
        encoderAngle += encoderDegreesPerStep * (float) steps;
        if (def.knob >= 0)
            ctx.mcu.sendKnob (def.knob, steps);
    }

    void KnobComponent::mouseDown (const juce::MouseEvent& e)
    {
        lastDragY = e.position.y;
        dragAccumulator = 0.0f;
    }

    void KnobComponent::mouseDrag (const juce::MouseEvent& e)
    {
        const float dy = lastDragY - e.position.y;
        lastDragY = e.position.y;
        if (encoder)
        {
            dragAccumulator += dy;
            const int steps = (int) (dragAccumulator / ctx.encoderPxPerStep);
            dragAccumulator -= (float) steps * ctx.encoderPxPerStep;
            step (steps);
        }
        else
        {
            setPotValue (potValue + dy / (body.getHeight() * 4.0f));
            if (onPotChanged)
                onPotChanged (potValue);
        }
    }

    void KnobComponent::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
    {
        // Trackpads deliver many small deltas; accumulate them into detents.
        wheelAccumulator += (w.deltaY + w.deltaX) * (w.isReversed ? -1.0f : 1.0f) * 8.0f;
        const int notches = (int) wheelAccumulator;
        wheelAccumulator -= (float) notches;
        if (notches == 0)
            return;
        if (encoder)
        {
            step (notches * ctx.wheelStepsPerNotch);
        }
        else
        {
            setPotValue (potValue + 0.02f * (float) notches);
            if (onPotChanged)
                onPotChanged (potValue);
        }
    }

    void KnobComponent::mouseDoubleClick (const juce::MouseEvent&)
    {
        if (! encoder)
        {
            setPotValue (0.5f);
            if (onPotChanged)
                onPotChanged (potValue);
        }
    }
}
