#include "panel/ScreenComponent.h"

namespace kickemu
{
    ScreenComponent::ScreenComponent (PanelContext& context, const LayoutControl& definition, SharedFramebuffer& framebuffer, QmpClient& client)
        : PanelControl (context, definition, 0.0f), fb (framebuffer), qmp (client)
    {
        absMax = ctx.config.getInt ("host.touch.abs_max", 0x7fff);
        maxContacts = juce::jlimit (1, 10, ctx.config.getInt ("host.touch.max_contacts", 5));
        keepAspect = ctx.config.getBool ("host.ui.keep_screen_pixel_aspect", true);
        sendAbs = ctx.config.getBool ("host.touch.send_abs", true);
        auto mod = ctx.config.getString ("host.touch.pinch_modifier", "alt");
        pinchModifier = mod == "ctrl" ? juce::ModifierKeys::ctrlModifier
                      : mod == "shift" ? juce::ModifierKeys::shiftModifier
                      : mod == "cmd" ? juce::ModifierKeys::commandModifier
                                     : juce::ModifierKeys::altModifier;
        setOpaque (true);
        setTooltip ("Touch screen: click and drag; hold " + mod + " for a two-finger pinch");
    }

    void ScreenComponent::setStatus (const juce::String& text)
    {
        if (status != text)
        {
            status = text;
            repaint();
        }
    }

    void ScreenComponent::rebuild()
    {
        // Glass reflection: a soft diagonal sheen, strongest at the top left.
        const int w = juce::roundToInt ((float) getWidth() * pixelScale), h = juce::roundToInt ((float) getHeight() * pixelScale);
        glare = juce::Image (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        juce::Graphics g (glare);
        g.addTransform (juce::AffineTransform::scale (pixelScale));
        auto b = body;
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.055f), b.getX(), b.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), b.getX() + b.getWidth() * 0.55f, b.getY() + b.getHeight() * 0.9f, false));
        juce::Path sheen;
        sheen.startNewSubPath (b.getX(), b.getY());
        sheen.lineTo (b.getX() + b.getWidth() * 0.62f, b.getY());
        sheen.lineTo (b.getX() + b.getWidth() * 0.30f, b.getBottom());
        sheen.lineTo (b.getX(), b.getBottom());
        sheen.closeSubPath();
        g.fillPath (sheen);
    }

    juce::Rectangle<float> ScreenComponent::imageArea() const
    {
        const int iw = fb.image.isValid() ? fb.image.getWidth() : 0, ih = fb.image.isValid() ? fb.image.getHeight() : 0;
        if (! keepAspect || iw <= 0 || ih <= 0)
            return body;
        // Follow the framebuffer's aspect, fitted into the active area.
        return juce::RectanglePlacement (juce::RectanglePlacement::centred).appliedTo (juce::Rectangle<float> ((float) iw, (float) ih), body);
    }

    bool ScreenComponent::animate (double, float)
    {
        const auto serial = fb.serial.load();
        const bool connected = fb.connected.load();
        if (serial == lastSerial && connected == lastConnected)
            return false;
        lastSerial = serial;
        lastConnected = connected;

        // Repaint only what the server changed.
        juce::Rectangle<int> dirty;
        juce::Rectangle<float> area;
        {
            std::lock_guard<std::mutex> guard (fb.lock);
            dirty = fb.dirty;
            fb.dirty = {};
            area = imageArea();
            if (fb.image.isValid() && ! dirty.isEmpty() && dirty != fb.image.getBounds())
            {
                const float sx = area.getWidth() / (float) fb.image.getWidth(), sy = area.getHeight() / (float) fb.image.getHeight();
                auto r = juce::Rectangle<float> (area.getX() + (float) dirty.getX() * sx, area.getY() + (float) dirty.getY() * sy,
                                                 (float) dirty.getWidth() * sx, (float) dirty.getHeight() * sy);
                repaint (r.expanded (2.0f).getSmallestIntegerContainer());
                return false;
            }
        }
        return true;
    }

    void ScreenComponent::paint (juce::Graphics& g)
    {
        g.fillAll (ctx.theme.glass);

        bool drewPicture = false;
        {
            std::lock_guard<std::mutex> guard (fb.lock);
            if (fb.connected.load() && fb.image.isValid())
            {
                g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                auto area = imageArea();
                g.drawImageTransformed (fb.image, juce::AffineTransform::scale (area.getWidth() / (float) fb.image.getWidth(),
                                                                                area.getHeight() / (float) fb.image.getHeight())
                                                      .translated (area.getX(), area.getY()));
                drewPicture = true;
            }
        }

        if (! drewPicture && status.isNotEmpty())
        {
            g.setColour (ctx.theme.labelOff);
            g.setFont (ctx.theme.uiFont (juce::jmax (10.0f, body.getHeight() * 0.045f)));
            g.drawText (status, body, juce::Justification::centred);
        }

        g.drawImageTransformed (glare, juce::AffineTransform::scale (1.0f / pixelScale));
    }

    juce::Point<int> ScreenComponent::toAxes (juce::Point<float> p) const
    {
        juce::Rectangle<float> area;
        {
            std::lock_guard<std::mutex> guard (const_cast<SharedFramebuffer&> (fb).lock);
            area = imageArea();
        }
        const float fx = juce::jlimit (0.0f, 1.0f, (p.x - area.getX()) / juce::jmax (1.0f, area.getWidth()));
        const float fy = juce::jlimit (0.0f, 1.0f, (p.y - area.getY()) / juce::jmax (1.0f, area.getHeight()));
        return { juce::roundToInt (fx * (float) absMax), juce::roundToInt (fy * (float) absMax) };
    }

    juce::var ScreenComponent::mtt (const char* type, int slot, int trackingId, const char* axis, int value) const
    {
        auto* data = new juce::DynamicObject();
        data->setProperty ("type", type);
        data->setProperty ("slot", slot);
        data->setProperty ("tracking-id", trackingId);
        data->setProperty ("axis", axis);
        data->setProperty ("value", value);
        auto* ev = new juce::DynamicObject();
        ev->setProperty ("type", "mtt");
        ev->setProperty ("data", juce::var (data));
        return juce::var (ev);
    }

    void ScreenComponent::sendContact (int slot, Phase phase, juce::Point<int> axes)
    {
        if (! juce::isPositiveAndBelow (slot, maxContacts))
            return;

        auto& c = contacts[(size_t) slot];
        if (phase == Phase::begin)
        {
            c.active = true;
            c.trackingId = nextTrackingId++ & 0xffff;
        }
        if (! c.active)
            return;
        c.axes = axes;

        // Same event sequence as QEMU's console_handle_touch_event(): for the
        // changed slot its own type, all other active slots get an update.
        juce::Array<juce::var> events;
        for (int i = 0; i < maxContacts; ++i)
        {
            auto& k = contacts[(size_t) i];
            if (! k.active)
                continue;
            const char* type = i == slot ? (phase == Phase::begin ? "begin" : phase == Phase::end ? "end" : "update") : "update";
            if (i == slot && phase == Phase::end)
            {
                events.add (mtt ("end", i, -1, "x", 0));
                continue;
            }
            events.add (mtt (type, i, k.trackingId, "x", 0));
            auto* btn = new juce::DynamicObject();
            btn->setProperty ("button", "touch");
            btn->setProperty ("down", true);
            auto* btnEv = new juce::DynamicObject();
            btnEv->setProperty ("type", "btn");
            btnEv->setProperty ("data", juce::var (btn));
            events.add (juce::var (btnEv));
            events.add (mtt ("data", i, k.trackingId, "x", k.axes.x));
            events.add (mtt ("data", i, k.trackingId, "y", k.axes.y));
        }

        if (phase == Phase::end)
            c = Contact();

        // Absolute pointer events for single-touch consumers (primary contact only).
        // Sent as a separate command: QEMU rejects a whole batch if no device
        // takes abs events, which must not cost us the multitouch events.
        juce::Array<juce::var> absEvents;
        if (sendAbs && slot == 0)
        {
            auto abs = [&absEvents] (const char* axis, int value)
            {
                auto* d = new juce::DynamicObject();
                d->setProperty ("axis", axis);
                d->setProperty ("value", value);
                auto* ev = new juce::DynamicObject();
                ev->setProperty ("type", "abs");
                ev->setProperty ("data", juce::var (d));
                absEvents.add (juce::var (ev));
            };
            abs ("x", axes.x);
            abs ("y", axes.y);
        }

        bool anyActive = false;
        for (auto& k : contacts)
            anyActive |= k.active;
        if (! anyActive)
        {
            auto* btn = new juce::DynamicObject();
            btn->setProperty ("button", "touch");
            btn->setProperty ("down", false);
            auto* btnEv = new juce::DynamicObject();
            btnEv->setProperty ("type", "btn");
            btnEv->setProperty ("data", juce::var (btn));
            events.add (juce::var (btnEv));
        }

        auto* args = new juce::DynamicObject();
        args->setProperty ("events", events);
        auto hint = ctx.onHint;
        qmp.execute ("input-send-event", juce::var (args), [hint] (const juce::var&, const juce::String& err)
        {
            if (err.isNotEmpty() && hint)
                juce::MessageManager::callAsync ([hint, err] { hint ("Touch: " + err); });
        });

        if (! absEvents.isEmpty())
        {
            auto* absArgs = new juce::DynamicObject();
            absArgs->setProperty ("events", absEvents);
            qmp.execute ("input-send-event", juce::var (absArgs),
                         [safe = juce::Component::SafePointer<ScreenComponent> (this), hint] (const juce::var&, const juce::String& err)
            {
                // No absolute pointer device in this machine: stop sending them.
                if (err.contains ("not found"))
                    juce::MessageManager::callAsync ([safe, hint]
                    {
                        if (safe != nullptr && safe->sendAbs)
                        {
                            safe->sendAbs = false;
                            if (hint)
                                hint ("Touch: no absolute pointer device, sending multitouch only");
                        }
                    });
            });
        }
    }

    void ScreenComponent::mouseDown (const juce::MouseEvent& e)
    {
        const int slot = e.source.getIndex() % maxContacts;
        sendContact (slot, Phase::begin, toAxes (e.position));
        pinching = e.mods.isAnyModifierKeyDown() && (e.mods.getRawFlags() & pinchModifier.getRawFlags()) != 0 && maxContacts > 1;
        if (pinching)
        {
            auto m = toAxes (body.getCentre() * 2.0f - e.position);
            sendContact ((slot + 1) % maxContacts, Phase::begin, m);
        }
    }

    void ScreenComponent::mouseDrag (const juce::MouseEvent& e)
    {
        const int slot = e.source.getIndex() % maxContacts;
        sendContact (slot, Phase::update, toAxes (e.position));
        if (pinching)
            sendContact ((slot + 1) % maxContacts, Phase::update, toAxes (body.getCentre() * 2.0f - e.position));
    }

    void ScreenComponent::mouseUp (const juce::MouseEvent& e)
    {
        const int slot = e.source.getIndex() % maxContacts;
        if (pinching)
            sendContact ((slot + 1) % maxContacts, Phase::end, toAxes (body.getCentre() * 2.0f - e.position));
        sendContact (slot, Phase::end, toAxes (e.position));
        pinching = false;
    }
}
