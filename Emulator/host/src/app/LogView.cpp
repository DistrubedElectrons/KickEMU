#include "app/LogView.h"

namespace kickemu
{
    LogView::LogView (LogBuffer& l, const Theme& t) : log (l), theme (t)
    {
        text.setMultiLine (true, false);
        text.setReadOnly (true);
        text.setScrollbarsShown (true);
        text.setCaretVisible (false);
        text.setFont (theme.monoFont (11.5f));
        text.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0e0e10));
        text.setColour (juce::TextEditor::textColourId, theme.chromeText);
        text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (text);
        addAndMakeVisible (clearButton);
        addAndMakeVisible (copyCommand);

        clearButton.onClick = [this] { log.clear(); cursor = 0; text.clear(); };
        copyCommand.onClick = [this]
        {
            if (getCommandLine)
                juce::SystemClipboard::copyTextToClipboard (getCommandLine());
        };
        startTimerHz (10);
    }

    void LogView::resized()
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (34).reduced (8, 5);
        clearButton.setBounds (bar.removeFromRight (64));
        bar.removeFromRight (6);
        copyCommand.setBounds (bar.removeFromRight (150));
        text.setBounds (r);
    }

    void LogView::timerCallback()
    {
        auto entries = log.since (cursor);
        if (entries.empty())
            return;

        juce::String chunk;
        for (auto& e : entries)
            chunk << e.time.formatted ("%H:%M:%S") << "  " << e.source.paddedRight (' ', 5) << "  " << e.text << "\n";

        // Keep the editor bounded; drop the oldest half when it gets long.
        if (text.getTotalNumChars() > 400000)
        {
            auto all = text.getText();
            text.setText (all.substring (all.length() / 2), false);
        }
        text.moveCaretToEnd();
        text.insertTextAtCaret (chunk);
    }
}
