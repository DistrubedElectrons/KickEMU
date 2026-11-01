#include "app/TerminalView.h"

namespace kickemu
{
    namespace
    {
        // xterm default palette (16 colours).
        const juce::uint32 ansi16[16] = {
            0xff1c1c1f, 0xffe5534b, 0xff57ab5a, 0xffc69026, 0xff539bf5, 0xffb083f0, 0xff39c5cf, 0xffc9cdd3,
            0xff636e7b, 0xffff938a, 0xff6bc46d, 0xffdaaa3f, 0xff6cb6ff, 0xffdcbdfb, 0xff56d4dd, 0xfff0f3f6
        };
    }

    TerminalView::TerminalView (ConsoleBridge& c, const Theme& t, float fontSize)
        : console (c), theme (t), font (t.monoFont (fontSize))
    {
        setWantsKeyboardFocus (true);
        setOpaque (true);
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, "MMMMMMMMMM", 0, 0);
        cellW = ga.getBoundingBox (0, -1, true).getWidth() / 10.0f;
        cellH = std::ceil (font.getHeight() * 1.12f);
        startTimerHz (30);
    }

    juce::Colour TerminalView::colourFor (int index, juce::uint32 rgb, bool bold) const
    {
        if (index == 255)
            return juce::Colour ((juce::uint32) (0xff000000u | rgb));
        if (index < 8 && bold)
            index += 8;
        if (index < 16)
            return juce::Colour (ansi16[index]);
        if (index < 232)
        {
            // 6x6x6 colour cube.
            const int i = index - 16;
            auto level = [] (int v) { return (juce::uint8) (v == 0 ? 0 : 55 + v * 40); };
            return juce::Colour (level (i / 36), level ((i / 6) % 6), level (i % 6));
        }
        const auto grey = (juce::uint8) (8 + (index - 232) * 10);
        return juce::Colour (grey, grey, grey);
    }

    void TerminalView::resized()
    {
        auto& model = console.getModel();
        model.resize (juce::jmax (20, (int) ((float) (getWidth() - 12) / cellW)), juce::jmax (5, (int) ((float) (getHeight() - 8) / cellH)));
    }

    void TerminalView::timerCallback()
    {
        auto rev = console.getModel().getRevision();
        const bool connected = console.isConnected();
        if (rev != lastRevision || connected != lastConnected)
        {
            if (rev != lastRevision)
                scrollOffset = 0;
            lastRevision = rev;
            lastConnected = connected;
            repaint();
        }
    }

    void TerminalView::paint (juce::Graphics& g)
    {
        const auto background = juce::Colour (0xff0d0d0f);
        g.fillAll (background);
        g.setFont (font);

        auto& model = console.getModel();
        std::lock_guard<std::mutex> guard (model.getLock());
        const auto& screen = model.getScreen();
        const auto& back = model.getScrollback();
        const int rows = model.getRows();
        scrollOffset = juce::jmin (scrollOffset, (int) back.size());

        for (int row = 0; row < rows; ++row)
        {
            // Rows above the screen come from the scrollback when scrolled.
            const int src = row - scrollOffset;
            const TerminalModel::Line* line = src >= 0 ? &screen[(size_t) src]
                                                       : &back[(size_t) ((int) back.size() + src)];
            const float y = 4.0f + (float) row * cellH;

            juce::String run;
            juce::Colour runColour;
            float runX = 0.0f;
            auto flush = [&]
            {
                if (run.isNotEmpty())
                {
                    g.setColour (runColour);
                    g.drawSingleLineText (run, juce::roundToInt (runX), juce::roundToInt (y + font.getAscent()));
                    run.clear();
                }
            };

            for (int col = 0; col < (int) line->size(); ++col)
            {
                const auto& cell = (*line)[(size_t) col];
                auto fg = cell.defaultFg ? colourFor (7, 0, cell.bold) : colourFor (cell.fg, cell.fgRgb, cell.bold);
                auto bg = cell.defaultBg ? background : colourFor (cell.bg, cell.bgRgb, false);
                if (cell.inverse)
                    std::swap (fg, bg);
                const float x = 6.0f + (float) col * cellW;
                if (bg != background)
                {
                    g.setColour (bg);
                    g.fillRect (x, y, cellW + 0.5f, cellH);
                }
                // Same-colour cells form one run; the font is monospaced so columns stay aligned.
                if (run.isNotEmpty() && fg != runColour)
                    flush();
                if (run.isEmpty())
                {
                    runColour = fg;
                    runX = x;
                }
                run << juce::String::charToString (cell.ch);
            }
            flush();
        }

        // Block cursor.
        if (scrollOffset == 0 && model.isCursorVisible())
        {
            auto cur = model.getCursor();
            auto r = juce::Rectangle<float> (6.0f + (float) cur.x * cellW, 4.0f + (float) cur.y * cellH, cellW, cellH);
            g.setColour (theme.accent.withAlpha (hasKeyboardFocus (false) ? 0.7f : 0.3f));
            if (hasKeyboardFocus (false))
                g.fillRect (r);
            else
                g.drawRect (r, 1.0f);
        }

        if (! console.isConnected())
        {
            g.setColour (theme.chromeDim);
            g.setFont (theme.uiFont (12.0f));
            g.drawText (console.getName() + " not connected", getLocalBounds().removeFromBottom (24).reduced (8, 0), juce::Justification::centredRight);
        }
    }

    void TerminalView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
    {
        scrollOffset = juce::jmax (0, scrollOffset + juce::roundToInt (w.deltaY * 12.0f));
        repaint();
    }

    bool TerminalView::keyPressed (const juce::KeyPress& key)
    {
        const auto code = key.getKeyCode();
        const auto mods = key.getModifiers();
        juce::String out;

        if (mods.isCommandDown())
        {
            if (code == 'V')
            {
                console.sendText (juce::SystemClipboard::getTextFromClipboard().replace ("\n", "\r"));
                return true;
            }
            return false; // app shortcuts
        }

        if (code == juce::KeyPress::returnKey) out = "\r";
        else if (code == juce::KeyPress::backspaceKey) out = "\x7f";
        else if (code == juce::KeyPress::tabKey) out = "\t";
        else if (code == juce::KeyPress::escapeKey) out = "\x1b";
        else if (code == juce::KeyPress::upKey) out = "\x1b[A";
        else if (code == juce::KeyPress::downKey) out = "\x1b[B";
        else if (code == juce::KeyPress::rightKey) out = "\x1b[C";
        else if (code == juce::KeyPress::leftKey) out = "\x1b[D";
        else if (code == juce::KeyPress::homeKey) out = "\x1b[H";
        else if (code == juce::KeyPress::endKey) out = "\x1b[F";
        else if (code == juce::KeyPress::deleteKey) out = "\x1b[3~";
        else if (code == juce::KeyPress::pageUpKey) out = "\x1b[5~";
        else if (code == juce::KeyPress::pageDownKey) out = "\x1b[6~";
        else if (mods.isCtrlDown() && code >= 'A' && code <= 'Z') out = juce::String::charToString ((juce::juce_wchar) (code - 'A' + 1));
        else if (key.getTextCharacter() >= ' ') out = juce::String::charToString (key.getTextCharacter());
        else return false;

        scrollOffset = 0;
        console.sendText (out);
        return true;
    }
}
