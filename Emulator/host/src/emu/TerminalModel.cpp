#include "emu/TerminalModel.h"

namespace kickemu
{
    TerminalModel::TerminalModel (int columns, int rowCount, int scrollbackLines)
        : cols (juce::jmax (10, columns)), rows (juce::jmax (2, rowCount)), maxScrollback (scrollbackLines)
    {
        screen.assign ((size_t) rows, Line ((size_t) cols));
        scrollBottom = rows - 1;
    }

    TerminalModel::Cell TerminalModel::blank() const
    {
        Cell c;
        c.bg = pen.bg;
        c.defaultBg = pen.defaultBg;
        c.bgRgb = pen.bgRgb;
        return c;
    }

    void TerminalModel::clear()
    {
        std::lock_guard<std::mutex> guard (lock);
        scrollback.clear();
        for (auto& line : screen)
            std::fill (line.begin(), line.end(), Cell());
        cx = cy = 0;
        wrapPending = false;
        revision++;
    }

    void TerminalModel::resize (int columns, int rowCount)
    {
        std::lock_guard<std::mutex> guard (lock);
        columns = juce::jmax (10, columns);
        rowCount = juce::jmax (2, rowCount);
        if (columns == cols && rowCount == rows)
            return;

        // Keep the bottom of the screen (where the prompt is) when shrinking.
        while ((int) screen.size() > rowCount)
        {
            if (cy > 0)
            {
                scrollback.push_back (screen.front());
                screen.erase (screen.begin());
                --cy;
            }
            else
            {
                screen.pop_back();
            }
        }
        while ((int) screen.size() < rowCount)
            screen.emplace_back ((size_t) columns);
        for (auto& line : screen)
            line.resize ((size_t) columns);

        cols = columns;
        rows = rowCount;
        scrollTop = 0;
        scrollBottom = rows - 1;
        cx = juce::jmin (cx, cols - 1);
        cy = juce::jmin (cy, rows - 1);
        while ((int) scrollback.size() > maxScrollback)
            scrollback.pop_front();
        revision++;
    }

    juce::String TerminalModel::screenText() const
    {
        juce::String out;
        for (auto& line : screen)
        {
            juce::String s;
            for (auto& c : line)
                s += juce::String::charToString (c.ch);
            out << s.trimEnd() << "\n";
        }
        return out;
    }

    int TerminalModel::param (int index, int fallback) const
    {
        auto parts = juce::StringArray::fromTokens (csiParams, ";", {});
        if (index >= parts.size() || parts[index].isEmpty())
            return fallback;
        return parts[index].getIntValue();
    }

    void TerminalModel::scrollUp (int top, int bottom, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (top == 0 && bottom == rows - 1)
            {
                scrollback.push_back (screen[(size_t) top]);
                if ((int) scrollback.size() > maxScrollback)
                    scrollback.pop_front();
            }
            screen.erase (screen.begin() + top);
            screen.insert (screen.begin() + bottom, Line ((size_t) cols, blank()));
        }
    }

    void TerminalModel::scrollDown (int top, int bottom, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            screen.erase (screen.begin() + bottom);
            screen.insert (screen.begin() + top, Line ((size_t) cols, blank()));
        }
    }

    void TerminalModel::newline()
    {
        wrapPending = false;
        if (cy == scrollBottom)
            scrollUp (scrollTop, scrollBottom, 1);
        else if (cy < rows - 1)
            ++cy;
    }

    void TerminalModel::put (juce::juce_wchar c)
    {
        if (wrapPending)
        {
            cx = 0;
            newline();
        }
        auto& cell = screen[(size_t) cy][(size_t) cx];
        cell = pen;
        cell.ch = c;
        if (cx == cols - 1)
            wrapPending = true;
        else
            ++cx;
    }

    void TerminalModel::eraseInLine (int mode)
    {
        auto& line = screen[(size_t) cy];
        int from = mode == 0 ? cx : 0;
        int to = mode == 1 ? cx : cols - 1;
        for (int x = from; x <= to && x < cols; ++x)
            line[(size_t) x] = blank();
    }

    void TerminalModel::eraseInDisplay (int mode)
    {
        if (mode == 0)
        {
            eraseInLine (0);
            for (int y = cy + 1; y < rows; ++y)
                std::fill (screen[(size_t) y].begin(), screen[(size_t) y].end(), blank());
        }
        else if (mode == 1)
        {
            eraseInLine (1);
            for (int y = 0; y < cy; ++y)
                std::fill (screen[(size_t) y].begin(), screen[(size_t) y].end(), blank());
        }
        else
        {
            for (auto& line : screen)
                std::fill (line.begin(), line.end(), blank());
            if (mode == 3)
                scrollback.clear();
        }
    }

    void TerminalModel::sgr()
    {
        auto parts = juce::StringArray::fromTokens (csiParams, ";", {});
        if (parts.isEmpty())
            parts.add ("0");

        for (int i = 0; i < parts.size(); ++i)
        {
            const int p = parts[i].getIntValue();
            if (p == 0) { pen = Cell(); }
            else if (p == 1) pen.bold = true;
            else if (p == 22) pen.bold = false;
            else if (p == 7) pen.inverse = true;
            else if (p == 27) pen.inverse = false;
            else if (p >= 30 && p <= 37) { pen.fg = (juce::uint8) (p - 30); pen.defaultFg = false; }
            else if (p == 39) { pen.fg = 7; pen.defaultFg = true; }
            else if (p >= 40 && p <= 47) { pen.bg = (juce::uint8) (p - 40); pen.defaultBg = false; }
            else if (p == 49) { pen.bg = 0; pen.defaultBg = true; }
            else if (p >= 90 && p <= 97) { pen.fg = (juce::uint8) (p - 90 + 8); pen.defaultFg = false; }
            else if (p >= 100 && p <= 107) { pen.bg = (juce::uint8) (p - 100 + 8); pen.defaultBg = false; }
            else if ((p == 38 || p == 48) && i + 1 < parts.size())
            {
                const bool fg = p == 38;
                const int kind = parts[i + 1].getIntValue();
                if (kind == 5 && i + 2 < parts.size())
                {
                    auto idx = (juce::uint8) parts[i + 2].getIntValue();
                    if (fg) { pen.fg = idx; pen.defaultFg = false; } else { pen.bg = idx; pen.defaultBg = false; }
                    i += 2;
                }
                else if (kind == 2 && i + 4 < parts.size())
                {
                    auto rgb = ((juce::uint32) parts[i + 2].getIntValue() << 16) | ((juce::uint32) parts[i + 3].getIntValue() << 8)
                               | (juce::uint32) parts[i + 4].getIntValue();
                    if (fg) { pen.fg = 255; pen.fgRgb = rgb; pen.defaultFg = false; } else { pen.bg = 255; pen.bgRgb = rgb; pen.defaultBg = false; }
                    i += 4;
                }
            }
        }
    }

    void TerminalModel::csi (juce::juce_wchar final)
    {
        const int n = juce::jmax (1, param (0, 1));
        wrapPending = false;

        switch (final)
        {
            case 'A': cy = juce::jmax (0, cy - n); break;
            case 'B': cy = juce::jmin (rows - 1, cy + n); break;
            case 'C': cx = juce::jmin (cols - 1, cx + n); break;
            case 'D': cx = juce::jmax (0, cx - n); break;
            case 'E': cy = juce::jmin (rows - 1, cy + n); cx = 0; break;
            case 'F': cy = juce::jmax (0, cy - n); cx = 0; break;
            case 'G': case '`': cx = juce::jlimit (0, cols - 1, param (0, 1) - 1); break;
            case 'd': cy = juce::jlimit (0, rows - 1, param (0, 1) - 1); break;
            case 'H': case 'f':
                cy = juce::jlimit (0, rows - 1, param (0, 1) - 1);
                cx = juce::jlimit (0, cols - 1, param (1, 1) - 1);
                break;
            case 'J': eraseInDisplay (param (0, 0)); break;
            case 'K': eraseInLine (param (0, 0)); break;
            case 'L': if (cy >= scrollTop && cy <= scrollBottom) scrollDown (cy, scrollBottom, n); break;
            case 'M': if (cy >= scrollTop && cy <= scrollBottom) scrollUp (cy, scrollBottom, n); break;
            case 'S': scrollUp (scrollTop, scrollBottom, n); break;
            case 'T': scrollDown (scrollTop, scrollBottom, n); break;
            case 'P':
            {
                auto& line = screen[(size_t) cy];
                for (int i = 0; i < n && cx < (int) line.size(); ++i)
                {
                    line.erase (line.begin() + cx);
                    line.push_back (blank());
                }
                break;
            }
            case '@':
            {
                auto& line = screen[(size_t) cy];
                for (int i = 0; i < n; ++i)
                {
                    line.insert (line.begin() + cx, blank());
                    line.pop_back();
                }
                break;
            }
            case 'X':
                for (int i = 0; i < n && cx + i < cols; ++i)
                    screen[(size_t) cy][(size_t) (cx + i)] = blank();
                break;
            case 'm': sgr(); break;
            case 'r':
                scrollTop = juce::jlimit (0, rows - 1, param (0, 1) - 1);
                scrollBottom = juce::jlimit (scrollTop, rows - 1, param (1, rows) - 1);
                cx = 0;
                cy = scrollTop;
                break;
            case 's': savedX = cx; savedY = cy; break;
            case 'u': cx = savedX; cy = savedY; break;
            case 'h': case 'l':
                if (csiPrivate && param (0, 0) == 25)
                    cursorVisible = final == 'h';
                break;
            default:
                break; // device status reports, modes etc. are not needed for display
        }
    }

    void TerminalModel::feed (const juce::uint8* data, size_t size)
    {
        std::lock_guard<std::mutex> guard (lock);

        for (size_t i = 0; i < size; ++i)
        {
            const auto b = data[i];

            // UTF-8 decoding (only in normal state; escape sequences are ASCII).
            juce::juce_wchar c = b;
            if (state == Parse::normal)
            {
                if (utf8Remaining > 0)
                {
                    if ((b & 0xc0) == 0x80)
                    {
                        utf8Value = (utf8Value << 6) | (b & 0x3f);
                        if (--utf8Remaining > 0)
                            continue;
                        c = (juce::juce_wchar) utf8Value;
                    }
                    else
                    {
                        utf8Remaining = 0; // invalid continuation: fall through with this byte
                    }
                }
                else if (b >= 0xc0 && b < 0xf8)
                {
                    utf8Remaining = b >= 0xf0 ? 3 : (b >= 0xe0 ? 2 : 1);
                    utf8Value = b & (b >= 0xf0 ? 0x07 : (b >= 0xe0 ? 0x0f : 0x1f));
                    continue;
                }
                else if (b >= 0x80)
                {
                    continue;
                }
            }

            switch (state)
            {
                case Parse::normal:
                    switch (c)
                    {
                        case 0x1b: state = Parse::escape; break;
                        case '\r': cx = 0; wrapPending = false; break;
                        case '\n': case 0x0b: case 0x0c: newline(); break;
                        case '\b': if (cx > 0) --cx; wrapPending = false; break;
                        case '\t': cx = juce::jmin (cols - 1, (cx / 8 + 1) * 8); break;
                        case 0x07: case 0x00: case 0x0e: case 0x0f: break;
                        default:
                            if (c >= 0x20 && c != 0x7f)
                                put (c);
                            break;
                    }
                    break;

                case Parse::escape:
                    state = Parse::normal;
                    switch (c)
                    {
                        case '[': state = Parse::csi; csiParams.clear(); csiPrivate = false; break;
                        case ']': state = Parse::osc; break;
                        case '(': case ')': state = Parse::charset; break;
                        case '7': savedX = cx; savedY = cy; break;
                        case '8': cx = savedX; cy = savedY; break;
                        case 'D': newline(); break;
                        case 'M': if (cy == scrollTop) scrollDown (scrollTop, scrollBottom, 1); else if (cy > 0) --cy; break;
                        case 'E': cx = 0; newline(); break;
                        case 'c':
                            pen = Cell();
                            for (auto& line : screen)
                                std::fill (line.begin(), line.end(), Cell());
                            cx = cy = 0;
                            scrollTop = 0;
                            scrollBottom = rows - 1;
                            break;
                        default: break;
                    }
                    break;

                case Parse::csi:
                    if (c == '?' || c == '>' || c == '=')
                        csiPrivate = true;
                    else if ((c >= '0' && c <= '9') || c == ';' || c == ':')
                        csiParams += juce::String::charToString (c == ':' ? ';' : c);
                    else if (c >= 0x40 && c <= 0x7e)
                    {
                        csi (c);
                        state = Parse::normal;
                    }
                    else if (c < 0x20 && c != 0x1b)
                    {
                        // C0 controls are executed inside CSI.
                        if (c == '\r') cx = 0;
                        else if (c == '\n') newline();
                        else if (c == '\b' && cx > 0) --cx;
                    }
                    else if (c == 0x1b)
                        state = Parse::escape;
                    break;

                case Parse::osc:
                    if (c == 0x07) state = Parse::normal;
                    else if (c == 0x1b) state = Parse::oscEscape;
                    break;

                case Parse::oscEscape:
                    state = c == '\\' ? Parse::normal : Parse::osc;
                    break;

                case Parse::charset:
                    state = Parse::normal;
                    break;
            }
        }
        revision++;
    }
}
