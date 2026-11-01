#pragma once

#include "app/Session.h"
#include "panel/Theme.h"

namespace kickemu
{
    /** Scrolling log of QEMU's output and host events (polled from the LogBuffer). */
    class LogView : public juce::Component, private juce::Timer
    {
    public:
        LogView (LogBuffer& log, const Theme& theme);
        void resized() override;
        std::function<juce::String()> getCommandLine;

    private:
        void timerCallback() override;

        LogBuffer& log;
        const Theme& theme;
        juce::TextEditor text;
        juce::TextButton clearButton { "Clear" }, copyCommand { "Copy QEMU command" };
        juce::uint64 cursor = 0;
    };
}
