#pragma once

#include <juce_core/juce_core.h>
#include <condition_variable>
#include <mutex>
#include <string>

namespace kickemu
{
    /**
        Keeps the text a console UART produced, addressed by absolute byte offsets,
        so automation clients can read "everything since offset N" and wait for a
        pattern without missing output that arrived between two calls.

        Escape sequences (CSI/OSC) and carriage returns are removed on the way in,
        so patterns match what a person reads on the terminal. Only the most recent
        `capacity` bytes are kept; offsets keep counting across the trimmed part.
        append() runs on the socket thread, everything else on any thread.
    */
    class ConsoleCapture
    {
    public:
        explicit ConsoleCapture (size_t capacityBytes = 4 * 1024 * 1024);

        void append (const juce::uint8* data, size_t size);
        void clear();

        /** Offset one past the last byte received. */
        juce::uint64 end() const;

        struct Chunk
        {
            juce::String text;
            juce::uint64 from = 0, to = 0;   // absolute offsets of text
            bool truncated = false;          // part of the requested range was already dropped
        };

        /** Text from `from` (clamped to what is still kept) to the end, at most maxBytes. */
        Chunk read (juce::uint64 from, size_t maxBytes = 65536) const;

        struct Match
        {
            bool found = false;
            juce::uint64 matchStart = 0, matchEnd = 0;   // absolute offsets
            juce::String text;                           // the matched text
            juce::String context;                        // the line holding the match
        };

        /** Searches for an ECMAScript regex in the text from `from` on. Blocks until it
            appears or timeoutMs elapses (0 = check once). The search restarts at the
            beginning of the line holding `from`, so a pattern split across two
            appends is still found. */
        Match waitFor (const juce::String& pattern, juce::uint64 from, int timeoutMs, juce::String& error) const;

        /** Wakes every waitFor() so it can re-check its timeout or cancellation. */
        void interrupt();

    private:
        juce::uint64 keptFrom() const { return total - (juce::uint64) text.size(); }

        mutable std::mutex lock;
        mutable std::condition_variable changed;
        std::string text;            // the kept tail, UTF-8 bytes
        juce::uint64 total = 0;      // filtered bytes ever appended
        size_t capacity;

        // Escape filter state carried across appends.
        enum class Esc { none, escape, csi, osc, oscEscape } esc = Esc::none;
    };
}
