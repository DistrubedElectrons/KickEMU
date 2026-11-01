#pragma once

#include <juce_graphics/juce_graphics.h>
#include <deque>
#include <mutex>
#include <vector>

namespace kickemu
{
    /**
        A small VT100/xterm-style terminal screen: enough of ANSI/ECMA-48 for a
        Linux shell, busybox tools and U-Boot (cursor movement, erase, colours,
        scroll regions). Bytes are fed from the console socket thread; the view
        reads under the same lock.
    */
    class TerminalModel
    {
    public:
        struct Cell
        {
            juce::juce_wchar ch = ' ';
            juce::uint8 fg = 7, bg = 0;   // palette indices, 7/0 = default
            bool bold = false, inverse = false, defaultFg = true, defaultBg = true;
            juce::uint32 fgRgb = 0, bgRgb = 0; // used when the index is 255 (24-bit colour)
        };
        using Line = std::vector<Cell>;

        TerminalModel (int columns, int rows, int scrollbackLines);

        void feed (const juce::uint8* data, size_t size);
        void resize (int columns, int rows);
        void clear();

        // Read access; hold getLock() while reading.
        std::mutex& getLock() { return lock; }
        int getColumns() const { return cols; }
        int getRows() const { return rows; }
        const std::deque<Line>& getScrollback() const { return scrollback; }
        const std::vector<Line>& getScreen() const { return screen; }
        juce::Point<int> getCursor() const { return { cx, cy }; }
        bool isCursorVisible() const { return cursorVisible; }
        juce::uint32 getRevision() const { return revision.load(); }

        /** Plain text of the screen (tests). */
        juce::String screenText() const;

    private:
        void put (juce::juce_wchar c);
        void newline();
        void scrollUp (int top, int bottom, int n);
        void scrollDown (int top, int bottom, int n);
        void csi (juce::juce_wchar final);
        void sgr();
        void eraseInLine (int mode);
        void eraseInDisplay (int mode);
        int param (int index, int fallback) const;
        Cell blank() const;

        std::mutex lock;
        int cols, rows, maxScrollback;
        std::vector<Line> screen;
        std::deque<Line> scrollback;
        int cx = 0, cy = 0, savedX = 0, savedY = 0;
        int scrollTop = 0, scrollBottom = 0;
        bool wrapPending = false, cursorVisible = true;
        Cell pen;

        enum class Parse { normal, escape, csi, osc, oscEscape, charset };
        Parse state = Parse::normal;
        juce::String csiParams;
        bool csiPrivate = false;

        // UTF-8 decoding state.
        juce::uint32 utf8Value = 0;
        int utf8Remaining = 0;

        std::atomic<juce::uint32> revision { 0 };
    };
}
