#include "automation/ConsoleCapture.h"

#include <regex>

namespace kickemu
{
    ConsoleCapture::ConsoleCapture (size_t capacityBytes) : capacity (juce::jmax ((size_t) 4096, capacityBytes)) {}

    void ConsoleCapture::append (const juce::uint8* data, size_t size)
    {
        {
            std::lock_guard<std::mutex> guard (lock);
            for (size_t i = 0; i < size; ++i)
            {
                const auto c = (char) data[i];
                switch (esc)
                {
                    case Esc::none:
                        if (c == '\x1b') esc = Esc::escape;
                        else if (c != '\r' && c != '\0') { text.push_back (c); ++total; }
                        break;
                    case Esc::escape:
                        esc = c == '[' ? Esc::csi : c == ']' ? Esc::osc : Esc::none;
                        break;
                    case Esc::csi:
                        // Parameter and intermediate bytes, then one final byte 0x40..0x7e.
                        if (c >= 0x40 && c <= 0x7e) esc = Esc::none;
                        break;
                    case Esc::osc:
                        if (c == '\x07') esc = Esc::none;
                        else if (c == '\x1b') esc = Esc::oscEscape;
                        break;
                    case Esc::oscEscape:
                        esc = c == '\\' ? Esc::none : Esc::osc;
                        break;
                }
            }
            if (text.size() > capacity)
                text.erase (0, text.size() - capacity);
        }
        changed.notify_all();
    }

    void ConsoleCapture::clear()
    {
        {
            std::lock_guard<std::mutex> guard (lock);
            // Offsets stay monotonic, so a client holding an old offset reads nothing stale.
            text.clear();
            esc = Esc::none;
        }
        changed.notify_all();
    }

    juce::uint64 ConsoleCapture::end() const
    {
        std::lock_guard<std::mutex> guard (lock);
        return total;
    }

    ConsoleCapture::Chunk ConsoleCapture::read (juce::uint64 from, size_t maxBytes) const
    {
        std::lock_guard<std::mutex> guard (lock);
        Chunk c;
        const auto first = keptFrom();
        c.truncated = from < first;
        c.from = juce::jlimit (first, total, from);
        const auto start = (size_t) (c.from - first);
        const auto n = juce::jmin (maxBytes, text.size() - start);
        c.text = juce::String::fromUTF8 (text.data() + start, (int) n);
        c.to = c.from + n;
        return c;
    }

    ConsoleCapture::Match ConsoleCapture::waitFor (const juce::String& pattern, juce::uint64 from, int timeoutMs, juce::String& error) const
    {
        Match m;
        std::regex re;
        try
        {
            re = std::regex (pattern.toStdString(), std::regex::ECMAScript);
        }
        catch (const std::regex_error& e)
        {
            error = "Invalid regular expression: " + juce::String (e.what());
            return m;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (juce::jmax (0, timeoutMs));
        std::unique_lock<std::mutex> guard (lock);
        juce::uint64 searchedTo = 0;
        for (;;)
        {
            if (total != searchedTo)
            {
                const auto first = keptFrom();
                const auto start = (size_t) (juce::jlimit (first, total, from) - first);
                // Back up to the start of the line so a line assembled from several
                // appends is matched as a whole.
                size_t base = 0;
                if (start > 0)
                    if (auto nl = text.rfind ('\n', start - 1); nl != std::string::npos)
                        base = nl + 1;

                std::smatch sm;
                const auto begin = text.cbegin() + (std::ptrdiff_t) base;
                if (std::regex_search (begin, text.cend(), sm, re))
                {
                    const auto pos = base + (size_t) sm.position (0);
                    // Only accept matches that end after `from`: earlier output is not new.
                    if (first + pos + (size_t) sm.length (0) > from || sm.length (0) == 0)
                    {
                        m.found = true;
                        m.matchStart = first + pos;
                        m.matchEnd = m.matchStart + (juce::uint64) sm.length (0);
                        m.text = juce::String::fromUTF8 (text.data() + pos, (int) sm.length (0));
                        const auto ls = text.rfind ('\n', pos == 0 ? 0 : pos - 1);
                        const auto lb = (ls == std::string::npos || pos == 0) ? 0 : ls + 1;
                        auto le = text.find ('\n', pos + (size_t) sm.length (0));
                        if (le == std::string::npos)
                            le = text.size();
                        m.context = juce::String::fromUTF8 (text.data() + lb, (int) (le - lb));
                        return m;
                    }
                }
                searchedTo = total;
            }
            if (timeoutMs <= 0 || changed.wait_until (guard, deadline) == std::cv_status::timeout)
            {
                if (total == searchedTo)
                    return m;
                timeoutMs = 0;   // one last search over what arrived with the timeout
            }
        }
    }

    void ConsoleCapture::interrupt() { changed.notify_all(); }
}
