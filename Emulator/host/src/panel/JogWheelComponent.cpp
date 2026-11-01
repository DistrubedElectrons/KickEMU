#include "panel/JogWheelComponent.h"
#include "panel/KnobComponent.h"

#include <contracts/dcl/dcl_contracts.h>

namespace kickemu
{
    using DC = contracts::DeviceControlContracts;

    namespace
    {
        constexpr int pushHoldMs = 250;   // holding the knob this long without turning = push

        float angleOf (juce::Point<float> c, juce::Point<float> p) { return std::atan2 (p.y - c.y, p.x - c.x); }
    }

    JogWheelComponent::JogWheelComponent (PanelContext& context, const LayoutControl& wheel, std::vector<const LayoutControl*> keys)
        : PanelControl (context, wheel, 3.0f)
    {
        // Order keys by quadrant: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
        keyDefs.assign (4, nullptr);
        for (auto* k : keys)
        {
            const int q = (k->quadrant.y > 0 ? 2 : 0) + (k->quadrant.x > 0 ? 1 : 0);
            keyDefs[(size_t) q] = k;
        }
        if (auto* leds = def.extra["ring_leds"].getArray())
            for (auto& l : *leds)
                ringLeds.push_back ((int) l);
        shownRing.assign (ringLeds.size(), 0.0f);
        ringColour = Theme::parseColour (def.extra["ring_colour"], juce::Colours::red);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool JogWheelComponent::hitTest (int x, int y) { return housing.contains ((float) x, (float) y); }

    int JogWheelComponent::quadrantAt (juce::Point<float> p) const
    {
        return (p.y > centre.y ? 2 : 0) + (p.x > centre.x ? 1 : 0);
    }

    void JogWheelComponent::rebuild()
    {
        housing = body;
        const auto origin = (def.getBounds() * pxPerMm).getTopLeft();
        const auto offset = body.getTopLeft() - origin;
        centre = def.centre * pxPerMm + offset;
        knobR = def.radius * pxPerMm;
        ringR = (float) (double) def.extra["ring_radius"] * pxPerMm;
        ringW = (float) (double) def.extra["ring_width"] * pxPerMm;
        recessR = ringR + ringW * 0.5f + mm (2.2f);

        auto& t = ctx.theme;
        const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);

        // Keys: the housing split by a cross-shaped gap, minus the round recess.
        const float gap = mm (0.7f), corner = mm (3.2f);
        juce::Path recess;
        recess.addEllipse (juce::Rectangle<float> (recessR * 2.0f, recessR * 2.0f).withCentre (centre));
        for (int q = 0; q < 4; ++q)
        {
            const bool right = (q & 1) != 0, bottom = (q & 2) != 0;
            juce::Rectangle<float> r (right ? centre.x + gap * 0.5f : housing.getX(), bottom ? centre.y + gap * 0.5f : housing.getY(),
                                      right ? housing.getRight() - centre.x - gap * 0.5f : centre.x - housing.getX() - gap * 0.5f,
                                      bottom ? housing.getBottom() - centre.y - gap * 0.5f : centre.y - housing.getY() - gap * 0.5f);
            juce::Path key;
            key.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), corner, corner,
                                     ! right && ! bottom, right && ! bottom, ! right && bottom, right && bottom);
            juce::Path hole;
            hole.addEllipse (juce::Rectangle<float> ((recessR + gap * 0.5f) * 2.0f, (recessR + gap * 0.5f) * 2.0f).withCentre (centre));
            key.addPath (hole);
            key.setUsingNonZeroWinding (false);
            keyPaths[(size_t) q] = key;
        }

        housingImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (housingImage);
            g.addTransform (juce::AffineTransform::scale (pixelScale));

            juce::Path base;
            base.addRoundedRectangle (housing, corner);
            paint::dropShadow (g, base, { 0.0f, mm (0.5f) }, mm (1.6f), 0.7f);
            g.setColour (juce::Colour (0xff0c0c0d));
            g.fillPath (base);

            for (int q = 0; q < 4; ++q)
            {
                auto& key = keyPaths[(size_t) q];
                auto kb = key.getBounds();
                g.setGradientFill (juce::ColourGradient (t.keycapTop, kb.getX(), kb.getY(), t.keycap.darker (0.25f), kb.getX(), kb.getBottom(), false));
                g.fillPath (key);
                paint::fillNoise (g, key, ctx.noise, 0.05f, pixelScale);
                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.strokePath (key, juce::PathStrokeType (mm (0.12f)), juce::AffineTransform::translation (0.0f, mm (0.08f)));
                g.setColour (juce::Colours::black.withAlpha (0.7f));
                g.strokePath (key, juce::PathStrokeType (mm (0.14f)));
            }

            // Printed key symbols in grey.
            g.setColour (t.labelOff.brighter (0.15f));
            const float iconSize = mm (3.2f);
            const juce::PathStrokeType stroke (mm (0.42f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            const float inset = mm (2.9f);
            for (int q = 0; q < 4; ++q)
            {
                if (keyDefs[(size_t) q] == nullptr)
                    continue;
                const bool right = (q & 1) != 0, bottom = (q & 2) != 0;
                juce::Point<float> ic (right ? housing.getRight() - inset - iconSize * 0.5f : housing.getX() + inset + iconSize * 0.5f,
                                       bottom ? housing.getBottom() - inset - iconSize * 0.5f : housing.getY() + inset + iconSize * 0.5f);
                auto box = juce::Rectangle<float> (iconSize, iconSize).withCentre (ic);
                const auto icon = keyDefs[(size_t) q]->icon;
                juce::Path p;
                if (icon == "undo")
                {
                    p.addCentredArc (box.getCentreX(), box.getCentreY() + box.getHeight() * 0.08f, box.getWidth() * 0.38f, box.getHeight() * 0.34f,
                                     0.0f, juce::degreesToRadians (-60.0f), juce::degreesToRadians (180.0f), true);
                    g.strokePath (p, stroke);
                    juce::Path head;
                    auto tip = juce::Point<float> (box.getX() + box.getWidth() * 0.17f, box.getY() + box.getHeight() * 0.18f);
                    head.addTriangle (tip.x - iconSize * 0.18f, tip.y + iconSize * 0.02f, tip.x + iconSize * 0.16f, tip.y - iconSize * 0.14f,
                                      tip.x + iconSize * 0.12f, tip.y + iconSize * 0.24f);
                    g.fillPath (head);
                }
                else if (icon == "copy")
                {
                    p.addRoundedRectangle (box.getX() + iconSize * 0.30f, box.getY(), iconSize * 0.62f, iconSize * 0.72f, iconSize * 0.08f);
                    g.strokePath (p, stroke);
                    juce::Path back;
                    back.startNewSubPath (box.getX() + iconSize * 0.16f, box.getY() + iconSize * 0.2f);
                    back.lineTo (box.getX() + iconSize * 0.16f, box.getY() + iconSize * 0.92f);
                    back.lineTo (box.getX() + iconSize * 0.72f, box.getY() + iconSize * 0.92f);
                    g.strokePath (back, stroke);
                }
                else if (icon == "minus" || icon == "plus")
                {
                    p.startNewSubPath (box.getX() + iconSize * 0.15f, box.getCentreY());
                    p.lineTo (box.getRight() - iconSize * 0.15f, box.getCentreY());
                    if (icon == "plus")
                    {
                        p.startNewSubPath (box.getCentreX(), box.getY() + iconSize * 0.15f);
                        p.lineTo (box.getCentreX(), box.getBottom() - iconSize * 0.15f);
                    }
                    g.strokePath (p, stroke);
                }
            }

            // Round recess with the dark ring groove.
            g.setColour (juce::Colour (0xff070708));
            g.fillPath (recess);
            paint::innerShadow (g, recess, mm (0.8f), 0.8f);
            g.setColour (juce::Colour (0xff151517));
            g.drawEllipse (juce::Rectangle<float> (ringR * 2.0f, ringR * 2.0f).withCentre (centre), ringW);
        }

        // Knob: a castellated skirt (rotates) and a brushed cone (fixed lighting).
        const float skirtR = knobR;
        skirtImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (skirtImage);
            g.addTransform (juce::AffineTransform::scale (pixelScale));
            juce::Path s;
            s.addEllipse (juce::Rectangle<float> (skirtR * 2.0f, skirtR * 2.0f).withCentre (centre));
            paint::dropShadow (g, s, { mm (0.3f), mm (0.6f) }, mm (1.5f), 0.8f);
            g.setColour (juce::Colour (0xff111113));
            g.fillPath (s);
            const int teeth = 24;
            for (int i = 0; i < teeth; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / (float) teeth;
                juce::Path tooth;
                const float half = juce::MathConstants<float>::pi / (float) teeth * 0.5f;
                tooth.startNewSubPath (centre.x + skirtR * 0.80f * std::cos (a - half), centre.y + skirtR * 0.80f * std::sin (a - half));
                tooth.lineTo (centre.x + skirtR * 0.99f * std::cos (a - half * 0.8f), centre.y + skirtR * 0.99f * std::sin (a - half * 0.8f));
                tooth.lineTo (centre.x + skirtR * 0.99f * std::cos (a + half * 0.8f), centre.y + skirtR * 0.99f * std::sin (a + half * 0.8f));
                tooth.lineTo (centre.x + skirtR * 0.80f * std::cos (a + half), centre.y + skirtR * 0.80f * std::sin (a + half));
                tooth.closeSubPath();
                const float lit = 0.5f + 0.5f * std::cos (a + juce::MathConstants<float>::pi * 0.75f);
                g.setColour (juce::Colour::greyLevel (0.12f + 0.16f * lit));
                g.fillPath (tooth);
            }
        }
        coneImage = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        {
            juce::Graphics g (coneImage);
            g.addTransform (juce::AffineTransform::scale (pixelScale));
            const float topR = skirtR * 0.82f;
            // drawCap draws skirt + top; use only its top part by clipping to the inner circle.
            juce::Path inner;
            inner.addEllipse (juce::Rectangle<float> (topR * 2.0f, topR * 2.0f).withCentre (centre));
            g.reduceClipRegion (inner);
            KnobComponent::drawCap (g, juce::Rectangle<float> (skirtR * 2.0f, skirtR * 2.0f).withCentre (centre), t, true, ctx.noise, pixelScale);
        }

        // Ring light: a continuous diffuse guide plus one hotspot per LED.
        juce::Path ringPath;
        ringPath.addEllipse (juce::Rectangle<float> (ringR * 2.0f, ringR * 2.0f).withCentre (centre));
        ringCore = paint::makeMask (ringPath, mm (0.35f), pixelScale, ringW * 0.55f);
        ringGlow = paint::makeMask (ringPath, mm (2.4f), pixelScale, ringW * 1.3f);
        hotspots.clear();
        for (size_t i = 0; i < ringLeds.size(); ++i)
        {
            const float a = -juce::MathConstants<float>::halfPi + juce::MathConstants<float>::twoPi * (float) i / (float) ringLeds.size();
            juce::Point<float> p (centre.x + ringR * std::cos (a), centre.y + ringR * std::sin (a));
            juce::Path dot;
            dot.addEllipse (juce::Rectangle<float> (ringW * 0.9f, ringW * 0.9f).withCentre (p));
            hotspots.push_back (paint::makeMask (dot, mm (0.9f), pixelScale));
        }
    }

    float JogWheelComponent::ringTarget (int index, double now) const
    {
        const int led = ringLeds[(size_t) index];
        if (juce::isPositiveAndBelow (led, PanelState::numLeds) && ctx.state.leds[(size_t) led].addressed.load())
            return ctx.state.ledAppearance (led, now, ctx.blinkPeriodSeconds).brightness;

        if (ctx.jogRingDefault == "always")
            return 1.0f;
        if (ctx.jogRingDefault == "connected")
            return ctx.state.uartConnected.load() ? 1.0f : 0.0f;
        return 0.0f;
    }

    bool JogWheelComponent::animate (double now, float dtMs)
    {
        bool changed = false;
        for (size_t i = 0; i < ringLeds.size(); ++i)
        {
            const float old = shownRing[i];
            shownRing[i] = approach (shownRing[i], ringTarget ((int) i, now), dtMs, ctx.fadeMs * 2.0f);
            changed |= std::abs (old - shownRing[i]) > 0.001f;
        }
        for (size_t q = 0; q < 4; ++q)
        {
            const float old = keyPress[q];
            keyPress[q] = approach (keyPress[q], keyDown[q] ? 1.0f : 0.0f, dtMs, keyDown[q] ? 10.0f : 40.0f);
            changed |= std::abs (old - keyPress[q]) > 0.001f;
        }
        const float oldAngle = shownWheelAngle, oldPush = pushAmount;
        shownWheelAngle = approach (shownWheelAngle, wheelAngle, dtMs, 20.0f);
        pushAmount = approach (pushAmount, pushHeld ? 1.0f : 0.0f, dtMs, 15.0f);
        return changed || std::abs (oldAngle - shownWheelAngle) > 0.0005f || std::abs (oldPush - pushAmount) > 0.001f;
    }

    void JogWheelComponent::paint (juce::Graphics& g)
    {
        const auto toLogical = juce::AffineTransform::scale (1.0f / pixelScale);
        g.drawImageTransformed (housingImage, toLogical);

        for (size_t q = 0; q < 4; ++q)
        {
            if (keyPress[q] > 0.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (0.28f * keyPress[q]));
                g.fillPath (keyPaths[q]);
            }
        }

        // Ring light under the knob so the knob's teeth catch the glow.
        float mean = 0.0f;
        for (auto v : shownRing)
            mean += v;
        mean = shownRing.empty() ? 0.0f : mean / (float) shownRing.size();
        const float ringLevel = paint::ledAlpha (mean);
        if (ringLevel > 0.0f)
        {
            ringGlow.draw (g, ringColour.withMultipliedAlpha (0.65f * ringLevel));
            ringCore.draw (g, ringColour.withMultipliedAlpha (0.85f * ringLevel));
        }
        for (size_t i = 0; i < hotspots.size(); ++i)
            if (shownRing[i] > 0.0f)
                hotspots[i].draw (g, ringColour.interpolatedWith (juce::Colour (0xffffd0c0), 0.35f).withMultipliedAlpha (paint::ledAlpha (shownRing[i])));

        {
            juce::Graphics::ScopedSaveState s (g);
            const float k = 1.0f - 0.025f * pushAmount;
            g.addTransform (juce::AffineTransform::scale (k, k, centre.x, centre.y));
            g.drawImageTransformed (skirtImage, toLogical.followedBy (juce::AffineTransform::rotation (shownWheelAngle, centre.x, centre.y)));

            // Red light leaking between the teeth.
            if (ringLevel > 0.0f)
            {
                juce::Path edge;
                edge.addEllipse (juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (centre));
                g.setColour (ringColour.withMultipliedAlpha (0.18f * ringLevel));
                g.strokePath (edge, juce::PathStrokeType (knobR * 0.12f));
            }
            g.drawImageTransformed (coneImage, toLogical);
        }
    }

    void JogWheelComponent::turn (int steps)
    {
        if (steps == 0)
            return;
        wheelAngle += juce::degreesToRadians (ctx.jogDegreesPerStep) * (float) steps;
        ctx.mcu.sendKnob (def.knob >= 0 ? def.knob : (int) DC::EN_KNOB_CENTER, steps);
    }

    void JogWheelComponent::setPushDown (bool isDown)
    {
        if (isDown == pushHeld)
            return;
        pushHeld = isDown;
        ctx.mcu.sendButton (def.pushHw >= 0 ? def.pushHw : (int) DC::EN_KNOB_ENTER, isDown ? DC::BTN_PRESSED : DC::BTN_RELEASED);
    }

    void JogWheelComponent::setKeyDown (int q, bool isDown)
    {
        if (! juce::isPositiveAndBelow (q, 4) || keyDefs[(size_t) q] == nullptr || keyDown[(size_t) q] == isDown)
            return;
        keyDown[(size_t) q] = isDown;
        ctx.mcu.sendButton (keyDefs[(size_t) q]->hw, isDown ? DC::BTN_PRESSED : DC::BTN_RELEASED);
    }

    void JogWheelComponent::mouseDown (const juce::MouseEvent& e)
    {
        const float d = e.position.getDistanceFrom (centre);
        if (d <= recessR)
        {
            drag = Drag::knob;
            lastAngle = angleOf (centre, e.position);
            angleAccumulator = 0.0f;
            turned = false;
            pushSent = false;
            downTime = juce::Time::getMillisecondCounter();
        }
        else
        {
            drag = Drag::key;
            dragKey = quadrantAt (e.position);
            setKeyDown (dragKey, true);
        }
    }

    void JogWheelComponent::mouseDrag (const juce::MouseEvent& e)
    {
        if (drag != Drag::knob)
            return;

        // Hold without turning: becomes a push.
        if (! turned && ! pushSent && juce::Time::getMillisecondCounter() - downTime > (juce::uint32) pushHoldMs
            && e.getDistanceFromDragStart() < 4)
        {
            pushSent = true;
            setPushDown (true);
            return;
        }
        if (pushSent || e.position.getDistanceFrom (centre) < knobR * 0.15f)
            return;

        float a = angleOf (centre, e.position);
        float delta = a - lastAngle;
        if (delta > juce::MathConstants<float>::pi) delta -= juce::MathConstants<float>::twoPi;
        if (delta < -juce::MathConstants<float>::pi) delta += juce::MathConstants<float>::twoPi;
        lastAngle = a;
        angleAccumulator += juce::radiansToDegrees (delta);

        const int steps = (int) (angleAccumulator / ctx.jogDegreesPerStep);
        if (steps != 0)
        {
            turned = true;
            angleAccumulator -= (float) steps * ctx.jogDegreesPerStep;
            turn (steps);
        }
    }

    void JogWheelComponent::mouseUp (const juce::MouseEvent&)
    {
        if (drag == Drag::knob)
        {
            if (pushSent)
            {
                setPushDown (false);
            }
            else if (! turned)
            {
                // Short click: press and release.
                setPushDown (true);
                juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<JogWheelComponent> (this)]
                {
                    if (safe != nullptr)
                        safe->setPushDown (false);
                });
            }
        }
        else if (drag == Drag::key)
        {
            setKeyDown (dragKey, false);
        }
        drag = Drag::none;
    }

    void JogWheelComponent::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
    {
        wheelAccumulator += (w.deltaY - w.deltaX) * (w.isReversed ? -1.0f : 1.0f) * 8.0f;
        const int notches = (int) wheelAccumulator;
        wheelAccumulator -= (float) notches;
        turn (-notches * ctx.wheelStepsPerNotch);
    }
}
