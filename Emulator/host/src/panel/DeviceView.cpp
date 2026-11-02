#include "panel/DeviceView.h"

namespace kickemu
{
    DeviceView::DeviceView (PanelContext& context, const Layout& l, SharedFramebuffer& fb, QmpClient& qmp, IoStatus& io)
        : ctx (context), layout (l), vblank (this, [this] { tick(); })
    {
        setWantsKeyboardFocus (true);
        setOpaque (false);
        pixelsPerMmAt100 = (float) ctx.config.getDouble ("host.ui.pixels_per_mm_at_100", 3.2);

        top = std::make_unique<TopPanel> (ctx, layout, fb, qmp);
        rear = std::make_unique<RearPanel> (ctx.theme, layout.rear, io);
        front = std::make_unique<FrontPanel> (ctx.theme, layout.front, io);
        addAndMakeVisible (*top);
        addChildComponent (*rear);
        addChildComponent (*front);

        // Keyboard bindings from layout.json ("key" on each control).
        for (auto* c : top->getControls())
        {
            auto& def = c->getDefinition();
            if (def.type == "jog")
            {
                jogPushKey = keyCodeFor (def.extra["key_push"].toString());
                jogCcwKey = keyCodeFor (def.extra["key_ccw"].toString());
                jogCwKey = keyCodeFor (def.extra["key_cw"].toString());
                continue;
            }
            if (def.key.isEmpty() || def.key == "SHIFT")
                continue;
            if (int code = keyCodeFor (def.key); code != 0)
                bindings.push_back ({ code, c });
        }
    }

    DeviceView::~DeviceView() = default;

    int DeviceView::keyCodeFor (const juce::String& name)
    {
        if (name.isEmpty())
            return 0;
        static const std::map<juce::String, int> named {
            { "SPACE", juce::KeyPress::spaceKey }, { "ESCAPE", juce::KeyPress::escapeKey },
            { "RETURN", juce::KeyPress::returnKey }, { "BACKSPACE", juce::KeyPress::backspaceKey },
            { "TAB", juce::KeyPress::tabKey }, { "DELETE", juce::KeyPress::deleteKey },
            { "UP", juce::KeyPress::upKey }, { "DOWN", juce::KeyPress::downKey },
            { "LEFT", juce::KeyPress::leftKey }, { "RIGHT", juce::KeyPress::rightKey },
            { "HOME", juce::KeyPress::homeKey }, { "END", juce::KeyPress::endKey },
            { "F1", juce::KeyPress::F1Key }, { "F2", juce::KeyPress::F2Key }, { "F3", juce::KeyPress::F3Key },
            { "F4", juce::KeyPress::F4Key }, { "F5", juce::KeyPress::F5Key }, { "F6", juce::KeyPress::F6Key },
            { "F7", juce::KeyPress::F7Key }, { "F8", juce::KeyPress::F8Key }, { "F9", juce::KeyPress::F9Key },
            { "F10", juce::KeyPress::F10Key }, { "F11", juce::KeyPress::F11Key }, { "F12", juce::KeyPress::F12Key },
        };
        if (auto it = named.find (name.toUpperCase()); it != named.end())
            return it->second;
        if (name.length() == 1)
            return (int) juce::CharacterFunctions::toUpperCase (name[0]);
        return 0;
    }

    juce::StringArray DeviceView::describeKeyboard() const
    {
        juce::StringArray lines;
        for (auto& b : bindings)
        {
            auto& d = b.control->getDefinition();
            auto name = d.tip.isNotEmpty() ? d.tip : (d.type == "pad" ? "Pad " + juce::String (d.index + 1) : d.id);
            lines.add (b.control->keyHint().paddedRight (' ', 10) + name);
        }
        lines.add (juce::String ("Shift").paddedRight (' ', 10) + "SHIFT");
        if (auto* j = top->getJog())
        {
            auto& d = j->getDefinition();
            lines.add ((d.extra["key_ccw"].toString() + "/" + d.extra["key_cw"].toString()).paddedRight (' ', 10) + "Jog wheel turn");
            lines.add (d.extra["key_push"].toString().paddedRight (' ', 10) + "Jog wheel push");
        }
        return lines;
    }

    void DeviceView::setView (View v)
    {
        releaseAllKeys();
        view = v;
        top->setVisible (v == View::top);
        rear->setVisible (v == View::rear);
        front->setVisible (v == View::front);
        resized();
    }

    void DeviceView::setScalePercent (int percent)
    {
        scalePercent = percent;
        resized();
    }

    double DeviceView::fitAspect() const
    {
        if (scalePercent > 0 || view != View::top)
            return 0.0;
        auto mm = top->getSizeMm();
        return mm.getHeight() > 0.0f ? (double) mm.getWidth() / (double) mm.getHeight() : 0.0;
    }

    juce::Rectangle<int> DeviceView::preferredSize (int percent) const
    {
        const float s = pixelsPerMmAt100 * (float) percent / 100.0f;
        auto mm = top->getSizeMm();
        return { juce::roundToInt (mm.getWidth() * s), juce::roundToInt (mm.getHeight() * s) };
    }

    void DeviceView::resized()
    {
        auto area = getLocalBounds().toFloat();
        auto fit = [&area, this] (float wMm, float hMm)
        {
            if (scalePercent > 0)
                return pixelsPerMmAt100 * (float) scalePercent / 100.0f;
            return juce::jmax (0.5f, juce::jmin (area.getWidth() / wMm, area.getHeight() / hMm));
        };

        if (view == View::top)
        {
            auto mm = top->getSizeMm();
            const float s = fit (mm.getWidth(), mm.getHeight());
            if (std::abs (s - top->getScale()) > 0.0005f || top->getWidth() == 0)
                top->setScale (s);
            top->setCentrePosition (getLocalBounds().getCentre());
        }
        else
        {
            StripPanel& p = view == View::rear ? (StripPanel&) *rear : (StripPanel&) *front;
            // The strips are long and thin; the same width scale as the top view keeps them 1:1.
            auto mm = top->getSizeMm();
            const float topScale = fit (mm.getWidth(), mm.getHeight());
            const float s = juce::jmin (topScale * 1.6f, area.getWidth() / p.getWidthMm(), area.getHeight() / p.getHeightMm());
            p.setScale (s);
            p.setCentrePosition (getLocalBounds().getCentre());
        }
    }

    juce::Image DeviceView::snapshot (float pixelScale)
    {
        if (view == View::top)
            return top->snapshot (pixelScale);
        auto& p = view == View::rear ? (juce::Component&) *rear : (juce::Component&) *front;
        return p.createComponentSnapshot (p.getLocalBounds(), true, pixelScale);
    }

    juce::Image DeviceView::grabFrame()
    {
        if (! isShowing())
            tick();     // no vblank without a peer
        auto& p = view == View::top ? (juce::Component&) *top
                : view == View::rear ? (juce::Component&) *rear : (juce::Component&) *front;
        return p.createComponentSnapshot (p.getLocalBounds(), true, juce::jmax (1.0f, top->getControlsPixelScale()));
    }

    void DeviceView::tick()
    {
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        const float dtMs = lastTick > 0.0 ? (float) juce::jlimit (0.0, 100.0, (now - lastTick) * 1000.0) : 16.0f;
        lastTick = now;

        switch (view)
        {
            case View::top: top->animate (now, dtMs); break;
            case View::rear: rear->animate (now, dtMs); break;
            case View::front: front->animate (now, dtMs); break;
        }
    }

    bool DeviceView::keyStateChanged (bool)
    {
        if (view != View::top)
            return false;

        // Shortcuts with Cmd/Ctrl belong to the app, not the device.
        auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
        if (mods.isCommandDown() || mods.isCtrlDown())
            return false;

        bool handled = false;
        for (auto& b : bindings)
        {
            const bool isDown = juce::KeyPress::isKeyCurrentlyDown (b.code);
            if (isDown != b.down)
            {
                b.down = isDown;
                b.control->setKeyDown (isDown);
                handled = true;
            }
        }

        if (auto* jog = top->getJog(); jog != nullptr && jogPushKey != 0)
        {
            const bool isDown = juce::KeyPress::isKeyCurrentlyDown (jogPushKey);
            if (isDown != jogPushDown)
            {
                jogPushDown = isDown;
                jog->setPushDown (isDown);
                handled = true;
            }
        }
        return handled;
    }

    bool DeviceView::keyPressed (const juce::KeyPress& key)
    {
        if (view != View::top || key.getModifiers().isCommandDown())
            return false;

        // Turning the jog wheel repeats with the key's auto-repeat.
        if (auto* jog = top->getJog())
        {
            if (jogCcwKey != 0 && key.getKeyCode() == jogCcwKey) { jog->turn (-1); return true; }
            if (jogCwKey != 0 && key.getKeyCode() == jogCwKey) { jog->turn (1); return true; }
        }

        for (auto& b : bindings)
            if (key.getKeyCode() == b.code || juce::CharacterFunctions::toUpperCase (key.getTextCharacter()) == (juce::juce_wchar) b.code)
                return true;
        return key.getKeyCode() == jogPushKey;
    }

    void DeviceView::modifierKeysChanged (const juce::ModifierKeys& mods)
    {
        if (view != View::top)
            return;
        const bool shift = mods.isShiftDown();
        if (shift == shiftKeyDown)
            return;
        shiftKeyDown = shift;
        for (auto* c : top->getControls())
            if (c->getDefinition().key == "SHIFT")
                c->setKeyDown (shift);
    }

    void DeviceView::releaseAllKeys()
    {
        for (auto& b : bindings)
        {
            if (b.down)
                b.control->setKeyDown (false);
            b.down = false;
        }
        if (jogPushDown && top->getJog() != nullptr)
            top->getJog()->setPushDown (false);
        jogPushDown = false;
        if (shiftKeyDown)
            modifierKeysChanged (juce::ModifierKeys());
    }

    void DeviceView::focusLost (FocusChangeType) { releaseAllKeys(); }
}
