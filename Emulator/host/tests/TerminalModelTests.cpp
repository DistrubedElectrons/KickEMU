#include "emu/TerminalModel.h"

using namespace kickemu;

class TerminalModelTests : public juce::UnitTest
{
public:
    TerminalModelTests() : juce::UnitTest ("Terminal model", "console") {}

    static void feed (TerminalModel& t, const char* s) { t.feed (reinterpret_cast<const juce::uint8*> (s), std::strlen (s)); }

    juce::String line (TerminalModel& t, int row)
    {
        return juce::StringArray::fromLines (t.screenText())[row];
    }

    void runTest() override
    {
        beginTest ("Text, CR/LF and wrapping");
        {
            TerminalModel t (10, 3, 100);
            feed (t, "hello\r\nworld");
            expectEquals (line (t, 0), juce::String ("hello"));
            expectEquals (line (t, 1), juce::String ("world"));
            feed (t, "\r\n0123456789AB");
            expectEquals (line (t, 2), juce::String ("AB"));
            expectEquals ((int) t.getScrollback().size(), 1);
        }

        beginTest ("Cursor positioning and erase");
        {
            TerminalModel t (20, 5, 100);
            feed (t, "abcdef\x1b[1;3HX\x1b[K");
            expectEquals (line (t, 0), juce::String ("abX"));
            feed (t, "\x1b[2J\x1b[3;2Hq");
            expectEquals (line (t, 2), juce::String (" q"));
            expectEquals (line (t, 0), juce::String());
        }

        beginTest ("Colours (SGR) incl. 256 and true colour");
        {
            TerminalModel t (20, 2, 10);
            feed (t, "\x1b[1;31mR\x1b[0m\x1b[38;5;200mP\x1b[48;2;1;2;3mT");
            auto& s = t.getScreen();
            expect (s[0][0].bold);
            expectEquals ((int) s[0][0].fg, 1);
            expectEquals ((int) s[0][1].fg, 200);
            expectEquals ((int) s[0][2].bg, 255);
            expectEquals ((int) s[0][2].bgRgb, 0x010203);
        }

        beginTest ("UTF-8 and backspace");
        {
            TerminalModel t (20, 2, 10);
            feed (t, "a\xc3\xa4" "b\bc");
            expectEquals (line (t, 0), juce::String (juce::CharPointer_UTF8 ("a\xc3\xa4" "c")));
        }

        beginTest ("Scroll region");
        {
            TerminalModel t (10, 4, 10);
            feed (t, "1\r\n2\r\n3\r\n4");
            feed (t, "\x1b[2;3r\x1b[3;1H\n");
            expectEquals (line (t, 0), juce::String ("1"));
            expectEquals (line (t, 1), juce::String ("3"));
            expectEquals (line (t, 3), juce::String ("4"));
        }
    }
};

static TerminalModelTests terminalModelTests;
