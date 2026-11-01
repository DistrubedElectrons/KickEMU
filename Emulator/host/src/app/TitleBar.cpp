#include "app/TitleBar.h"

namespace kickemu
{
    namespace icons
    {
        static juce::PathStrokeType line (float w) { return juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded); }

        juce::Path play (juce::Rectangle<float> r)
        {
            juce::Path p;
            auto b = r.reduced (r.getWidth() * 0.22f);
            p.addTriangle (b.getX() + b.getWidth() * 0.12f, b.getY(), b.getX() + b.getWidth() * 0.12f, b.getBottom(), b.getRight(), b.getCentreY());
            return p.createPathWithRoundedCorners (r.getWidth() * 0.06f);
        }

        juce::Path stop (juce::Rectangle<float> r)
        {
            juce::Path p;
            p.addRoundedRectangle (r.reduced (r.getWidth() * 0.27f), r.getWidth() * 0.06f);
            return p;
        }

        juce::Path restart (juce::Rectangle<float> r)
        {
            juce::Path arc, out;
            auto c = r.getCentre();
            const float rad = r.getWidth() * 0.28f;
            arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, juce::degreesToRadians (40.0f), juce::degreesToRadians (330.0f), true);
            line (r.getWidth() * 0.09f).createStrokedPath (out, arc);
            const float a = juce::degreesToRadians (40.0f - 90.0f);
            juce::Point<float> tip (c.x + rad * std::cos (a), c.y + rad * std::sin (a));
            out.addTriangle (tip.x - rad * 0.45f, tip.y - rad * 0.2f, tip.x + rad * 0.3f, tip.y - rad * 0.45f, tip.x + rad * 0.12f, tip.y + rad * 0.35f);
            return out;
        }

        juce::Path camera (juce::Rectangle<float> r)
        {
            juce::Path p, out;
            auto b = r.reduced (r.getWidth() * 0.2f, r.getHeight() * 0.26f);
            p.addRoundedRectangle (b, b.getHeight() * 0.18f);
            p.addEllipse (juce::Rectangle<float> (b.getHeight() * 0.6f, b.getHeight() * 0.6f).withCentre (b.getCentre()));
            line (r.getWidth() * 0.07f).createStrokedPath (out, p);
            out.addRectangle (b.getX() + b.getWidth() * 0.18f, b.getY() - b.getHeight() * 0.18f, b.getWidth() * 0.24f, b.getHeight() * 0.18f);
            return out;
        }

        juce::Path expand (juce::Rectangle<float> r)
        {
            juce::Path p, out;
            auto b = r.reduced (r.getWidth() * 0.26f);
            const float k = b.getWidth() * 0.32f;
            p.startNewSubPath (b.getX(), b.getY() + k); p.lineTo (b.getX(), b.getY()); p.lineTo (b.getX() + k, b.getY());
            p.startNewSubPath (b.getRight() - k, b.getY()); p.lineTo (b.getRight(), b.getY()); p.lineTo (b.getRight(), b.getY() + k);
            p.startNewSubPath (b.getRight(), b.getBottom() - k); p.lineTo (b.getRight(), b.getBottom()); p.lineTo (b.getRight() - k, b.getBottom());
            p.startNewSubPath (b.getX() + k, b.getBottom()); p.lineTo (b.getX(), b.getBottom()); p.lineTo (b.getX(), b.getBottom() - k);
            line (r.getWidth() * 0.08f).createStrokedPath (out, p);
            return out;
        }

        juce::Path sidebar (juce::Rectangle<float> r)
        {
            juce::Path p, out;
            auto b = r.reduced (r.getWidth() * 0.22f, r.getHeight() * 0.27f);
            p.addRoundedRectangle (b, r.getWidth() * 0.05f);
            p.startNewSubPath (b.getRight() - b.getWidth() * 0.34f, b.getY());
            p.lineTo (b.getRight() - b.getWidth() * 0.34f, b.getBottom());
            line (r.getWidth() * 0.07f).createStrokedPath (out, p);
            return out;
        }
    }

    // ------------------------------------------------------------------------------------

    IconButton::IconButton (const juce::String& name, const Theme& t, std::function<juce::Path (juce::Rectangle<float>)> icon)
        : juce::Button (name), theme (t), makeIcon (std::move (icon))
    {
        setTooltip (name);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        if (over || down || highlighted)
        {
            g.setColour (highlighted ? theme.accent.withAlpha (0.18f) : juce::Colours::white.withAlpha (down ? 0.10f : 0.06f));
            g.fillRoundedRectangle (r, 6.0f);
        }
        g.setColour (highlighted ? theme.accent : (over ? theme.chromeText : theme.chromeText.withAlpha (0.75f)));
        if (makeIcon)
            g.fillPath (makeIcon (r));
    }

    // ------------------------------------------------------------------------------------

    Segmented::Segmented (const Theme& t, juce::StringArray i) : theme (t), items (std::move (i))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void Segmented::setSelected (int index)
    {
        selected = juce::jlimit (0, items.size() - 1, index);
        repaint();
    }

    void Segmented::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (theme.chromeLine);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

        const float w = r.getWidth() / (float) items.size();
        for (int i = 0; i < items.size(); ++i)
        {
            auto cell = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight());
            if (i == selected)
            {
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.fillRoundedRectangle (cell.reduced (2.0f), (cell.getHeight() - 4.0f) * 0.5f);
            }
            g.setColour (i == selected ? theme.chromeText : theme.chromeDim);
            g.setFont (theme.uiFont (12.5f, i == selected));
            g.drawText (items[i], cell, juce::Justification::centred);
        }
    }

    void Segmented::mouseUp (const juce::MouseEvent& e)
    {
        const int index = juce::jlimit (0, items.size() - 1, (int) (e.position.x / ((float) getWidth() / (float) items.size())));
        if (index != selected)
        {
            setSelected (index);
            if (onChange)
                onChange (index);
        }
    }

    // ------------------------------------------------------------------------------------

    TitleBar::TitleBar (const Theme& t)
        : views (t, { "Top", "Rear", "Front" }),
          power ("Start / stop QEMU", t, icons::play),
          restart ("Restart QEMU", t, icons::restart),
          snapshot ("Save a PNG snapshot of the device (Cmd+Shift+S)", t, icons::camera),
          presentation ("Presentation mode (Cmd+P)", t, icons::expand),
          panel ("Console, log and settings (Cmd+L)", t, icons::sidebar),
          theme (t)
    {
        addAndMakeVisible (views);
        addAndMakeVisible (scale);
        for (auto* b : { &power, &restart, &snapshot, &presentation, &panel })
            addAndMakeVisible (b);
        scale.setTooltip ("Device scale");
    }

    void TitleBar::setStatus (const Status& s)
    {
        status = s;
        repaint (statusArea);
    }

    void TitleBar::paint (juce::Graphics& g)
    {
        g.fillAll (theme.chrome);
        g.setColour (theme.chromeLine);
        g.fillRect (getLocalBounds().removeFromBottom (1));

        auto r = getLocalBounds().reduced (14, 0);
        g.setColour (theme.chromeText);
        g.setFont (theme.uiFont (13.5f, true));
        g.drawText ("KickEmu", r.removeFromLeft (70), juce::Justification::centredLeft);

        // Status pill.
        auto area = statusArea.toFloat();
        auto ind = area.removeFromRight (124.0f).withTrimmedLeft (10.0f);
        auto pill = area.reduced (0.0f, 9.0f);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ pill.getX() + 12.0f, pill.getCentreY() });
        g.setColour (status.qemuColour);
        g.fillEllipse (dot);
        g.setColour (status.qemuColour.withAlpha (0.25f));
        g.fillEllipse (dot.expanded (3.0f));

        juce::String text = status.qemu;
        if (status.detail.isNotEmpty())
            text << "  " << status.detail;
        g.setColour (theme.chromeText);
        g.setFont (theme.uiFont (12.0f));
        auto textArea = pill.withTrimmedLeft (22.0f).withTrimmedRight (8.0f);
        g.drawText (text, textArea, juce::Justification::centredLeft, true);

        // Link indicators: UART1/MCU and display.
        auto indicator = [&] (juce::Rectangle<float> a, const juce::String& label, bool on, bool partial)
        {
            auto d = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ a.getX() + 4.0f, a.getCentreY() });
            g.setColour (on ? theme.ok : (partial ? theme.warn : theme.chromeDim.withAlpha (0.5f)));
            g.fillEllipse (d);
            g.setColour (theme.chromeDim);
            g.setFont (theme.uiFont (11.5f));
            g.drawText (label, a.withTrimmedLeft (11.0f), juce::Justification::centredLeft);
        };
        indicator (ind.removeFromLeft (52.0f), "MCU", status.mcuLinked, status.uartConnected);
        indicator (ind.removeFromLeft (64.0f), "Display", status.displayConnected, false);
    }

    void TitleBar::resized()
    {
        auto r = getLocalBounds().reduced (10, 0);
        r.removeFromLeft (76);
        statusArea = r.removeFromLeft (juce::jmin (420, juce::jmax (220, getWidth() / 2 - 160)));

        auto right = r.removeFromRight (5 * 34 + 92);
        panel.setBounds (right.removeFromRight (34).withSizeKeepingCentre (32, 30));
        presentation.setBounds (right.removeFromRight (34).withSizeKeepingCentre (32, 30));
        snapshot.setBounds (right.removeFromRight (34).withSizeKeepingCentre (32, 30));
        restart.setBounds (right.removeFromRight (34).withSizeKeepingCentre (32, 30));
        power.setBounds (right.removeFromRight (34).withSizeKeepingCentre (32, 30));
        scale.setBounds (right.removeFromRight (86).withSizeKeepingCentre (80, 24));

        views.setBounds (getLocalBounds().withSizeKeepingCentre (180, 26)
                             .withX (juce::jmax (statusArea.getRight() + 8, getWidth() / 2 - 90)));
        // Only show the segmented control when it fits between status and actions.
        views.setVisible (views.getRight() < scale.getX() - 8);
        // Status pill takes what's left of its column (indicators need ~120px).
        statusArea.setWidth (juce::jmax (0, juce::jmin (statusArea.getWidth(), views.getX() - statusArea.getX() - 8)));
    }
}
