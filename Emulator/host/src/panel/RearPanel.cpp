#include "panel/RearPanel.h"

namespace kickemu
{
    StripPanel::StripPanel (const Theme& t, const LayoutStrip& s, IoStatus& st)
        : theme (t), strip (s), status (st)
    {
        setOpaque (false);
    }

    void StripPanel::setScale (float pxPerMm)
    {
        scale = pxPerMm;
        setSize (juce::roundToInt (getWidthMm() * scale), juce::roundToInt (getHeightMm() * scale));
        staticLayer = {};
        repaint();
    }

    void StripPanel::paint (juce::Graphics& g)
    {
        const float ps = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
        if (! staticLayer.isValid() || std::abs (ps - staticPixelScale) > 0.01f)
        {
            staticPixelScale = ps;
            staticLayer = juce::Image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) getWidth() * ps)),
                                       juce::jmax (1, juce::roundToInt ((float) getHeight() * ps)), true);
            juce::Graphics sg (staticLayer);
            sg.addTransform (juce::AffineTransform::scale (ps));
            drawStatic (sg);
        }
        g.drawImageTransformed (staticLayer, juce::AffineTransform::scale (1.0f / staticPixelScale));
        drawIndicators (g);
    }

    void StripPanel::drawStatic (juce::Graphics& g) const
    {
        auto body = px (juce::Rectangle<float> (0.0f, 0.0f, strip.width, strip.height));

        // Feet.
        for (auto& foot : strip.feet)
        {
            auto r = px (juce::Rectangle<float> (foot.x, strip.height - 0.5f, foot.y, strip.footHeight + 0.5f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff18181a), r.getX(), r.getY(), juce::Colour (0xff050505), r.getX(), r.getBottom(), false));
            g.fillRoundedRectangle (r, 0.6f * scale);
        }

        juce::Path shape;
        shape.addRoundedRectangle (body.getX(), body.getY(), body.getWidth(), body.getHeight(), 1.4f * scale, 1.4f * scale, true, true, false, false);
        paint::dropShadow (g, shape, { 0.0f, 1.2f * scale }, 3.0f * scale, 0.6f);
        g.setGradientFill (juce::ColourGradient (theme.enclosure.brighter (0.12f), body.getX(), body.getY(),
                                                 theme.enclosure.darker (0.3f), body.getX(), body.getBottom(), false));
        g.fillPath (shape);

        // The top cover's edge catches the light.
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.drawLine (body.getX() + scale, body.getY() + 0.15f * scale, body.getRight() - scale, body.getY() + 0.15f * scale, 0.3f * scale);

        if (! strip.panel.isEmpty())
        {
            auto p = px (strip.panel);
            juce::Path panel;
            panel.addRoundedRectangle (p, 0.6f * scale);
            g.setColour (theme.enclosure.darker (0.15f));
            g.fillPath (panel);
            paint::innerShadow (g, panel, 0.5f * scale, 0.5f);
        }

        g.setColour (theme.labelPrint.withAlpha (0.55f));
        for (auto x : strip.dividers)
        {
            auto a = px (juce::Point<float> (x, strip.dividerTop)), b = px (juce::Point<float> (x, strip.dividerBottom));
            g.drawLine (a.x, a.y, b.x, b.y, 0.18f * scale);
        }

        for (auto& s : strip.screws)
        {
            const float r = strip.screwRadius * scale;
            auto c = px (s);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3b3f), c.x - r, c.y - r, juce::Colour (0xff111113), c.x + r, c.y + r, false));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
            g.setColour (juce::Colour (0xff060606));
            g.drawLine (c.x - r * 0.55f, c.y, c.x + r * 0.55f, c.y, r * 0.22f);
            g.drawLine (c.x, c.y - r * 0.55f, c.x, c.y + r * 0.55f, r * 0.22f);
        }

        for (auto& c : strip.connectors)
            drawConnector (g, c);

        for (auto& t : strip.texts)
        {
            auto font = t.font == "heading" ? theme.headingFont (t.size * scale) : theme.labelFont (t.size * scale);
            g.setFont (font);
            g.setColour (theme.labelPrint);
            auto pos = px (t.position);
            g.drawText (t.text, juce::Rectangle<float> (pos.x - 40.0f * scale, pos.y - font.getHeight() * 0.5f, 80.0f * scale, font.getHeight()),
                        juce::Justification::centred, false);
        }

        if (strip.wordmark.text.isNotEmpty())
        {
            auto font = theme.wordmarkFont (strip.wordmark.size * scale);
            g.setFont (font);
            g.setColour (theme.wordmark);
            auto pos = px (strip.wordmark.position);
            g.drawText (strip.wordmark.text, juce::Rectangle<float> (pos.x - 60.0f * scale, pos.y - font.getHeight() * 0.5f, 120.0f * scale, font.getHeight()),
                        juce::Justification::centred, false);
        }
    }

    void StripPanel::drawConnector (juce::Graphics& g, const LayoutConnector& c) const
    {
        auto centre = px (c.centre);
        auto metalRing = [&g] (juce::Point<float> ce, float r, float thickness)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe4e6ea), ce.x - r, ce.y - r, juce::Colour (0xff5d6066), ce.x + r, ce.y + r, false));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (ce));
            g.setColour (juce::Colour (0xff050505));
            g.fillEllipse (juce::Rectangle<float> ((r - thickness) * 2.0f, (r - thickness) * 2.0f).withCentre (ce));
        };

        if (c.type == "din")
        {
            const float r = c.radius * scale;
            metalRing (centre, r, r * 0.14f);
            g.setColour (juce::Colour (0xff141416));
            g.fillEllipse (juce::Rectangle<float> (r * 1.5f, r * 1.5f).withCentre (centre));
            g.setColour (juce::Colour (0xffa9acb2));
            for (int i = 0; i < 5; ++i)
            {
                const float a = juce::degreesToRadians (180.0f + 45.0f * (float) i);
                juce::Point<float> p (centre.x + r * 0.42f * std::cos (a), centre.y + r * 0.42f * std::sin (a));
                g.fillEllipse (juce::Rectangle<float> (r * 0.12f, r * 0.12f).withCentre (p));
            }
            g.setColour (juce::Colour (0xff050505));
            g.fillRect (juce::Rectangle<float> (r * 0.16f, r * 0.2f).withCentre ({ centre.x, centre.y - r * 0.66f }));
        }
        else if (c.type == "jack" || c.type == "mini_jack")
        {
            const float r = c.radius * scale;
            g.setColour (juce::Colour (0xff0d0d0e));
            g.fillEllipse (juce::Rectangle<float> (r * 2.3f, r * 2.3f).withCentre (centre));
            metalRing (centre, r, r * 0.38f);
            g.setColour (juce::Colour (0xff000000));
            g.fillEllipse (juce::Rectangle<float> (r * 0.9f, r * 0.9f).withCentre (centre));
        }
        else if (c.type == "rj45" || c.type == "usb_a" || c.type == "usb_c" || c.type == "dc_jack")
        {
            auto r = juce::Rectangle<float> (c.size.x * scale, c.size.y * scale).withCentre (centre);
            const float corner = c.type == "usb_c" ? r.getHeight() * 0.5f : 0.3f * scale;
            if (c.type == "dc_jack")
            {
                g.setColour (juce::Colour (0xff0b0b0c));
                g.fillRoundedRectangle (r, 0.4f * scale);
                g.setColour (juce::Colour (0xff000000));
                g.fillEllipse (juce::Rectangle<float> (r.getWidth() * 0.62f, r.getWidth() * 0.62f).withCentre (centre));
                g.setColour (juce::Colour (0xff9a9da3));
                g.fillEllipse (juce::Rectangle<float> (r.getWidth() * 0.18f, r.getWidth() * 0.18f).withCentre (centre));
                return;
            }
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd9dbe0), r.getX(), r.getY(), juce::Colour (0xff6c6f75), r.getRight(), r.getBottom(), false));
            g.fillRoundedRectangle (r, corner);
            g.setColour (juce::Colour (0xff060606));
            g.fillRoundedRectangle (r.reduced (r.getHeight() * (c.type == "usb_c" ? 0.2f : 0.12f)), corner * 0.8f);
            if (c.type == "usb_a")
            {
                g.setColour (juce::Colour (0xff1d1d20));
                g.fillRect (r.reduced (r.getWidth() * 0.16f, r.getHeight() * 0.35f).withY (r.getY() + r.getHeight() * 0.3f));
            }
            else if (c.type == "rj45")
            {
                g.setColour (juce::Colour (0xffb08a2a));
                for (int i = 0; i < 8; ++i)
                    g.fillRect (r.getX() + r.getWidth() * (0.22f + 0.075f * (float) i), r.getY() + r.getHeight() * 0.25f, r.getWidth() * 0.035f, r.getHeight() * 0.18f);
            }
            else if (c.type == "usb_c")
            {
                g.setColour (juce::Colour (0xff2a2a2e));
                g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.18f, r.getHeight() * 0.38f), r.getHeight() * 0.1f);
            }
        }
        else if (c.type == "power_button")
        {
            const float r = c.radius * scale;
            g.setColour (juce::Colour (0xff060607));
            g.fillEllipse (juce::Rectangle<float> (r * 2.2f, r * 2.2f).withCentre (centre));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2e2f33), centre.x - r, centre.y - r, juce::Colour (0xff0d0d0f), centre.x + r, centre.y + r, false));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (centre));
        }
        else if (c.type == "sd_slot")
        {
            auto r = px (c.rect);
            g.setColour (juce::Colour (0xff030303));
            g.fillRoundedRectangle (r, r.getHeight() * 0.3f);
        }
    }

    void StripPanel::drawLed (juce::Graphics& g, juce::Point<float> c, float sizeMm, juce::Colour colour, float level) const
    {
        const float r = sizeMm * scale * 0.5f;
        g.setColour (juce::Colour (0xff0a0a0a));
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
        if (level <= 0.0f)
            return;
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.55f * level), c.x, c.y, colour.withAlpha (0.0f), c.x + r * 4.0f, c.y, true));
        g.fillEllipse (juce::Rectangle<float> (r * 8.0f, r * 8.0f).withCentre (c));
        g.setColour (colour.interpolatedWith (juce::Colours::white, 0.4f).withAlpha (level));
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
    }

    const LayoutConnector* StripPanel::connectorAt (juce::Point<float> p) const
    {
        for (auto& c : strip.connectors)
        {
            juce::Rectangle<float> r;
            if (! c.rect.isEmpty()) r = px (c.rect);
            else if (c.radius > 0.0f) r = juce::Rectangle<float> (c.radius * 2.4f * scale, c.radius * 2.4f * scale).withCentre (px (c.centre));
            else r = juce::Rectangle<float> (c.size.x * scale, c.size.y * scale).withCentre (px (c.centre));
            if (r.expanded (scale).contains (p))
                return &c;
        }
        return nullptr;
    }

    void StripPanel::mouseMove (const juce::MouseEvent& e)
    {
        auto* c = connectorAt (e.position);
        setTooltip (c != nullptr ? describe (*c) : juce::String());
        setMouseCursor (c != nullptr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void StripPanel::mouseUp (const juce::MouseEvent& e)
    {
        if (auto* c = connectorAt (e.position); c != nullptr && status.onConnectorClicked)
            status.onConnectorClicked (c->id);
    }

    // ------------------------------------------------------------------------------------

    RearPanel::RearPanel (const Theme& t, const LayoutStrip& s, IoStatus& st) : StripPanel (t, s, st) {}

    void RearPanel::animate (double, float dtMs)
    {
        bool changed = false;
        auto pulse = [&] (std::function<juce::uint64()>& count, juce::uint64& last, float& glow)
        {
            const auto n = count ? count() : 0;
            const float old = glow;
            if (n != last)
            {
                last = n;
                glow = 1.0f;
            }
            else
            {
                glow = juce::jmax (0.0f, glow - dtMs / 120.0f);
            }
            changed |= std::abs (old - glow) > 0.001f;
        };
        pulse (status.midiInCount, lastIn, inGlow);
        pulse (status.midiOutCount, lastOut, outGlow);

        const int channels = status.cvChannels ? juce::jmin (16, status.cvChannels()) : 0;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float v = status.cvOut ? status.cvOut (ch) : 0.0f;
            changed |= std::abs (v - cvShown[(size_t) ch]) > 0.001f;
            cvShown[(size_t) ch] = v;
        }

        const bool powered = status.powered && status.powered();
        const bool net = status.networkUp && status.networkUp();
        changed |= powered != lastPowered || net != lastNet;
        lastPowered = powered;
        lastNet = net;

        if (changed)
            repaint();
    }

    void RearPanel::drawIndicators (juce::Graphics& g)
    {
        for (auto& c : strip.connectors)
        {
            auto centre = px (c.centre);
            if (c.id == "midi_in" || c.id == "midi_out")
            {
                const bool in = c.id == "midi_in";
                const float r = c.radius * scale;
                drawLed (g, { centre.x + r * 0.78f, centre.y - r * 0.98f }, 1.4f, in ? juce::Colour (0xff35ff7a) : juce::Colour (0xffffb233),
                         in ? inGlow : outGlow);
            }
            else if (c.id == "link")
            {
                auto r = juce::Rectangle<float> (c.size.x * scale, c.size.y * scale).withCentre (centre);
                drawLed (g, { r.getX() + r.getWidth() * 0.14f, r.getY() + r.getHeight() * 0.12f }, 1.2f, juce::Colour (0xff3dff6a), lastNet ? 1.0f : 0.0f);
            }
            else if (c.type == "mini_jack" && c.channel >= 0)
            {
                // CV readout under the jack: output of the channel and of its pair (n, n+4).
                const float r = c.radius * scale;
                const int a = c.channel, b = c.channel + 4;
                auto text = juce::String (cvShown[(size_t) a], 2) + " V";
                if (status.cvChannels && b < status.cvChannels())
                    text << "\n" << juce::String (cvShown[(size_t) b], 2) + " V";
                auto font = theme.monoFont (2.1f * scale);
                g.setFont (font);
                const float level = juce::jlimit (0.0f, 1.0f, juce::jmax (cvShown[(size_t) a], cvShown[(size_t) b]) / 5.0f);
                g.setColour (juce::Colour (0xff4fd2ff).withAlpha (0.45f + 0.55f * level));
                auto box = juce::Rectangle<float> (centre.x - r * 2.0f, centre.y + r * 1.25f, r * 4.0f, font.getHeight() * 2.1f);
                paint::drawLines (g, text, font, box, juce::Justification::centred, 1.0f);
            }
            else if (c.id == "power")
            {
                const float r = c.radius * scale;
                if (lastPowered)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.10f));
                    g.drawEllipse (juce::Rectangle<float> (r * 1.6f, r * 1.6f).withCentre (centre), 0.2f * scale);
                }
            }
        }
    }

    juce::String RearPanel::describe (const LayoutConnector& c) const
    {
        if (c.id == "midi_in")
            return "MIDI IN (UART3): " + (status.midiInRoute ? status.midiInRoute() : juce::String()) + ", "
                   + juce::String ((juce::int64) lastIn) + " messages";
        if (c.id == "midi_out")
            return "MIDI OUT (UART3): " + (status.midiOutRoute ? status.midiOutRoute() : juce::String()) + ", "
                   + juce::String ((juce::int64) lastOut) + " messages";
        if (c.id == "link")
            return "LINK (Ethernet): " + (status.networkInfo ? status.networkInfo() : juce::String());
        if (c.type == "mini_jack" && c.channel >= 0)
        {
            juce::String s = "CV/GATE " + juce::String (c.channel + 1) + "/" + juce::String (c.channel + 5) + ": out "
                             + juce::String (cvShown[(size_t) c.channel], 3) + " V / " + juce::String (cvShown[(size_t) c.channel + 4], 3) + " V";
            if (status.cvIn)
                s << ", in " << juce::String (status.cvIn (c.channel), 2) << " V / " << juce::String (status.cvIn (c.channel + 4), 2) << " V";
            s << (status.cvConnected && status.cvConnected() ? "" : " (converter not connected)") << ". Click to set inputs.";
            return s;
        }
        if (c.type == "jack")
            return (c.id.startsWith ("out") ? "OUTPUT " : "INPUT ") + c.id.getLastCharacters (1).toUpperCase() + ": "
                   + (status.audioRoute ? status.audioRoute() : juce::String());
        if (c.id == "power")
            return lastPowered ? "Power: on (QEMU running)" : "Power: off";
        if (c.id == "usb_c" || c.id == "usb_a")
            return "USB: not emulated yet";
        if (c.id == "dc_in")
            return "DC IN 12 V";
        return c.id;
    }

    // ------------------------------------------------------------------------------------

    FrontPanel::FrontPanel (const Theme& t, const LayoutStrip& s, IoStatus& st) : StripPanel (t, s, st) {}

    void FrontPanel::animate (double, float)
    {
        auto sd = status.sdImage ? status.sdImage() : juce::String();
        const bool powered = status.powered && status.powered();
        if (sd != lastSd || powered != lastPowered)
        {
            lastSd = sd;
            lastPowered = powered;
            repaint();
        }
    }

    void FrontPanel::drawIndicators (juce::Graphics& g)
    {
        for (auto& c : strip.connectors)
        {
            if (c.type != "sd_slot" || lastSd.isEmpty())
                continue;
            // A card in the slot: its edge sits just inside the opening.
            auto r = px (c.rect).reduced (0.6f * scale, 0.15f * scale);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2c5fd4), r.getX(), r.getY(), juce::Colour (0xff17327a), r.getX(), r.getBottom(), false));
            g.fillRoundedRectangle (r, r.getHeight() * 0.2f);
        }
    }

    juce::String FrontPanel::describe (const LayoutConnector& c) const
    {
        if (c.type == "sd_slot")
            return lastSd.isEmpty() ? "SD card slot: empty (set storage.sd_image)" : "SD card: " + lastSd;
        if (c.id == "phones")
            return "PHONES: " + (status.audioRoute ? status.audioRoute() : juce::String());
        return c.id;
    }
}
