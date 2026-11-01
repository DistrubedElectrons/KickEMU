#include "panel/TopPanel.h"

namespace kickemu
{
    TopPanel::TopPanel (PanelContext& context, const Layout& l, SharedFramebuffer& framebuffer, QmpClient& qmp)
        : ctx (context), layout (l)
    {
        setOpaque (false);
        std::vector<const LayoutControl*> jogKeys = layout.ofType ("jog_key");

        for (auto& c : layout.controls)
        {
            PanelControl* control = nullptr;
            if (c.type == "pad")
                control = new PadComponent (ctx, c);
            else if (c.type == "button" || c.type == "round_button")
                control = new ButtonComponent (ctx, c);
            else if (c.type == "pot" || c.type == "encoder")
                control = new KnobComponent (ctx, c);
            else if (c.type == "slider")
                control = new SliderComponent (ctx, c);
            else if (c.type == "led")
                control = new LedComponent (ctx, c);
            else if (c.type == "jog")
                control = jog = new JogWheelComponent (ctx, c, jogKeys);
            else if (c.type == "screen")
                control = screen = new ScreenComponent (ctx, c, framebuffer, qmp);

            if (control != nullptr)
            {
                owned.add (control);
                controls.push_back (control);
                addAndMakeVisible (control);
            }
        }
    }

    juce::Rectangle<float> TopPanel::getSizeMm() const
    {
        return { layout.width + 2.0f * shadowMarginMm, layout.height + 2.0f * shadowMarginMm };
    }

    juce::Rectangle<float> TopPanel::toPx (juce::Rectangle<float> r) const
    {
        return (r.translated (shadowMarginMm, shadowMarginMm)) * scale;
    }

    juce::Point<float> TopPanel::toPx (juce::Point<float> p) const
    {
        return (p + juce::Point<float> (shadowMarginMm, shadowMarginMm)) * scale;
    }

    void TopPanel::setScale (float pxPerMm)
    {
        scale = pxPerMm;
        auto size = getSizeMm() * scale;
        setSize (juce::roundToInt (size.getWidth()), juce::roundToInt (size.getHeight()));

        placeControls (juce::jmax (1.0f, juce::Component::getApproximateScaleFactorForComponent (this)));
        staticLayer = {};
        repaint();
    }

    void TopPanel::placeControls (float ps)
    {
        controlsPixelScale = ps;
        for (auto* c : controls)
        {
            c->place (scale, ps);
            c->setTopLeftPosition (c->getPosition() + juce::Point<int> (juce::roundToInt (shadowMarginMm * scale),
                                                                       juce::roundToInt (shadowMarginMm * scale)));
        }
    }

    void TopPanel::animate (double now, float dtMs)
    {
        for (auto* c : controls)
            if (c->animate (now, dtMs))
                c->repaint();
    }

    PanelControl* TopPanel::controlForKey (const juce::String& key) const
    {
        for (auto* c : controls)
            if (c->getDefinition().key.equalsIgnoreCase (key))
                return c;
        return nullptr;
    }

    KnobComponent* TopPanel::getPot (const juce::String& id) const
    {
        for (auto* c : controls)
            if (c->getDefinition().id == id)
                return dynamic_cast<KnobComponent*> (c);
        return nullptr;
    }

    KnobComponent* TopPanel::getEncoder (int index) const
    {
        for (auto* c : controls)
            if (c->getDefinition().type == "encoder" && c->getDefinition().knob == index)
                return dynamic_cast<KnobComponent*> (c);
        return nullptr;
    }

    // ------------------------------------------------------------------------------------
    // Static layer

    void TopPanel::paint (juce::Graphics& g)
    {
        const float ps = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        if (! staticLayer.isValid() || std::abs (ps - staticPixelScale) > 0.01f)
        {
            staticPixelScale = ps;
            staticLayer = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * ps)),
                                       juce::jmax (1, juce::roundToInt ((float) getHeight() * ps)), true);
            juce::Graphics sg (staticLayer);
            sg.addTransform (juce::AffineTransform::scale (ps));
            renderStatic (sg);
        }
        g.drawImageTransformed (staticLayer, juce::AffineTransform::scale (1.0f / staticPixelScale));

        // The control caches follow the display's pixel density (e.g. moving to a Retina screen).
        if (std::abs (ps - controlsPixelScale) > 0.01f && ! snapshotting)
        {
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TopPanel> (this), ps]
            {
                if (safe != nullptr)
                    safe->placeControls (ps);
            });
            controlsPixelScale = ps;
        }
    }

    void TopPanel::renderStatic (juce::Graphics& g) const
    {
        drawEnclosure (g);
        for (auto& d : layout.decor)
            drawDecor (g, d);
        drawWells (g);
        drawTexts (g);
    }

    void TopPanel::drawEnclosure (juce::Graphics& g) const
    {
        auto& t = ctx.theme;
        auto body = toPx (juce::Rectangle<float> (0.0f, 0.0f, layout.width, layout.height));
        const float radius = layout.cornerRadius * scale;
        juce::Path shape;
        shape.addRoundedRectangle (body, radius);

        // Contact shadow plus a wide soft shadow under the device.
        paint::dropShadow (g, shape, { 0.0f, 3.0f * scale }, 7.0f * scale, 0.65f);
        paint::dropShadow (g, shape, { 0.0f, 0.8f * scale }, 1.6f * scale, 0.8f);

        // Matte soft-touch top: gentle light falloff from the top left, fine grain.
        g.setGradientFill (juce::ColourGradient (t.enclosure.brighter (0.10f), body.getX(), body.getY(),
                                                 t.enclosure.darker (0.18f), body.getRight(), body.getBottom(), false));
        g.fillPath (shape);
        paint::fillNoise (g, shape, ctx.noise, 0.055f, 1.0f);

        // Bevelled edge: a narrow chamfer, lit along the top/left, dark along the bottom/right.
        const float bevel = layout.bevel * scale;
        juce::Path inner;
        inner.addRoundedRectangle (body.reduced (bevel), juce::jmax (0.0f, radius - bevel * 0.6f));
        juce::Path chamfer (shape);
        chamfer.addPath (inner);
        chamfer.setUsingNonZeroWinding (false);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (chamfer);
            juce::ColourGradient grad (t.bevelLight.withAlpha (0.55f), body.getX(), body.getY(),
                                       juce::Colours::black.withAlpha (0.55f), body.getRight(), body.getBottom(), false);
            grad.addColour (0.45, t.enclosureEdge.withAlpha (0.35f));
            g.setGradientFill (grad);
            g.fillPath (chamfer);
        }

        // Specular highlight on the outer edge, strongest along the top.
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.22f), body.getCentreX(), body.getY(),
                                                 juce::Colours::white.withAlpha (0.02f), body.getCentreX(), body.getBottom(), false));
        g.strokePath (shape, juce::PathStrokeType (0.25f * scale));
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.strokePath (inner, juce::PathStrokeType (0.12f * scale));
    }

    void TopPanel::drawDecor (juce::Graphics& g, const LayoutDecor& d) const
    {
        auto& t = ctx.theme;
        if (d.type == "recess")
        {
            auto r = toPx (d.rect);
            juce::Path p;
            p.addRoundedRectangle (r, 1.0f * scale);
            g.setColour (t.recess);
            g.fillPath (p);
            paint::fillNoise (g, p, ctx.noise, 0.04f, 1.0f);
            paint::innerShadow (g, p, 0.9f * scale, 0.85f);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawLine (r.getX() + scale, r.getBottom() + 0.1f * scale, r.getRight() - scale, r.getBottom() + 0.1f * scale, 0.2f * scale);
        }
        else if (d.type == "bezel")
        {
            auto r = toPx (d.rect);
            juce::Path p;
            p.addRoundedRectangle (r, 0.8f * scale);
            paint::dropShadow (g, p, { 0.0f, 0.3f * scale }, 0.8f * scale, 0.7f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff121214), r.getX(), r.getY(),
                                                     juce::Colour (0xff070708), r.getX(), r.getBottom(), false));
            g.fillPath (p);
            // Glass edge highlight and a faint reflection across the bezel.
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.strokePath (p, juce::PathStrokeType (0.15f * scale));
            juce::Path sheen;
            sheen.startNewSubPath (r.getX(), r.getY());
            sheen.lineTo (r.getX() + r.getWidth() * 0.45f, r.getY());
            sheen.lineTo (r.getX() + r.getWidth() * 0.18f, r.getBottom());
            sheen.lineTo (r.getX(), r.getBottom());
            sheen.closeSubPath();
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.035f), r.getX(), r.getY(),
                                                     juce::Colours::transparentWhite, r.getX() + r.getWidth() * 0.4f, r.getBottom(), false));
            g.fillPath (sheen);
        }
        else if (d.type == "camera")
        {
            auto r = toPx (d.rect);
            juce::Path p;
            p.addRoundedRectangle (r, 0.8f * scale);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff141416), r.getX(), r.getY(), juce::Colour (0xff09090a), r.getX(), r.getBottom(), false));
            g.fillPath (p);
            paint::innerShadow (g, p, 0.5f * scale, 0.6f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.strokePath (p, juce::PathStrokeType (0.12f * scale));

            // Sensor window (amber) and lens.
            auto sensor = juce::Rectangle<float> (2.4f * scale, 3.2f * scale).withCentre (toPx (d.extra.hasProperty ("sensor")
                ? juce::Point<float> ((float) (double) d.extra["sensor"][0], (float) (double) d.extra["sensor"][1]) : d.rect.getCentre()));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a6a2c), sensor.getX(), sensor.getY(), juce::Colour (0xff4a3512), sensor.getX(), sensor.getBottom(), false));
            g.fillRoundedRectangle (sensor, 0.3f * scale);

            auto lensC = toPx (juce::Point<float> ((float) (double) d.extra["lens"][0], (float) (double) d.extra["lens"][1]));
            const float lr = (float) (double) d.extra["lens_radius"] * scale;
            g.setColour (juce::Colour (0xff1c1c22));
            g.fillEllipse (juce::Rectangle<float> (lr * 2.0f, lr * 2.0f).withCentre (lensC));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c3170), lensC.x - lr * 0.3f, lensC.y - lr * 0.3f,
                                                     juce::Colour (0xff06060c), lensC.x + lr * 0.6f, lensC.y + lr * 0.6f, true));
            g.fillEllipse (juce::Rectangle<float> (lr * 1.4f, lr * 1.4f).withCentre (lensC));
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.fillEllipse (juce::Rectangle<float> (lr * 0.32f, lr * 0.32f).withCentre (lensC.translated (-lr * 0.25f, -lr * 0.25f)));
        }
        else if (d.type == "line")
        {
            g.setColour (t.labelPrint.withAlpha (0.9f));
            g.drawLine ({ toPx (d.a), toPx (d.b) }, 0.28f * scale);
        }
        else if (d.type == "dot")
        {
            g.setColour (t.labelPrint);
            g.fillEllipse (juce::Rectangle<float> (d.radius * 2.0f * scale, d.radius * 2.0f * scale).withCentre (toPx (d.a)));
        }
    }

    void TopPanel::drawWells (juce::Graphics& g) const
    {
        // Dark gaps around keys and pads, where they sit in the top plate.
        for (auto& c : layout.controls)
        {
            juce::Path well;
            if (c.type == "pad")
                well.addRoundedRectangle (toPx (c.rect.expanded (0.55f)), 1.4f * scale);
            else if (c.type == "button")
                well.addRoundedRectangle (toPx (c.rect.expanded (0.35f)), 1.3f * scale);
            else if (c.type == "round_button")
                well.addEllipse (toPx (c.getBounds().expanded (0.45f)));
            else if (c.type == "slider")
            {
                juce::Path strip;
                auto r = toPx (c.rect);
                strip.addRoundedRectangle (r.withRight (r.getX() + r.getWidth() * 0.78f).reduced (0.9f * scale, 0.6f * scale), 0.6f * scale);
                g.setColour (juce::Colour (0xff1c1d20));
                g.fillPath (strip);
                paint::innerShadow (g, strip, 0.5f * scale, 0.5f);
                continue;
            }
            else
                continue;

            g.setColour (juce::Colour (0xff080809));
            g.fillPath (well);
            paint::innerShadow (g, well, 0.3f * scale, 0.9f);
        }
    }

    void TopPanel::drawTexts (juce::Graphics& g) const
    {
        auto& t = ctx.theme;
        for (auto& text : layout.texts)
        {
            const auto pos = toPx (text.position);
            const float size = text.size * scale;
            juce::Font font = text.font == "wordmark" ? t.wordmarkFont (size)
                            : text.font == "heading" ? t.headingFont (size)
                                                     : t.labelFont (size);
            juce::Colour colour = text.colour == "wordmark" ? t.wordmark
                                : text.colour == "wordmark_bright" ? t.wordmark.brighter (0.1f)
                                                                   : t.labelPrint;
            g.setFont (font);
            g.setColour (colour);
            const float w = juce::GlyphArrangement::getStringWidth (font, text.text) + 2.0f;
            const float h = font.getHeight();
            auto box = text.align == "left" ? juce::Rectangle<float> (pos.x, pos.y - h * 0.5f, w, h)
                                            : juce::Rectangle<float> (pos.x - w * 0.5f, pos.y - h * 0.5f, w, h);
            g.drawText (text.text, box, text.align == "left" ? juce::Justification::centredLeft : juce::Justification::centred, false);
        }
    }

    juce::Image TopPanel::snapshot (float ps)
    {
        // Re-render the control caches at the snapshot resolution so it stays crisp.
        const float previous = controlsPixelScale;
        snapshotting = true;
        placeControls (ps);
        auto image = createComponentSnapshot (getLocalBounds(), true, ps);
        snapshotting = false;
        placeControls (previous);
        staticLayer = {};
        return image;
    }
}
