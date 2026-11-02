#include "instruments/MidiMonitorView.h"

namespace kickemu
{
    namespace
    {
        constexpr int maxRows = 2000;
        constexpr int headerHeight = 18;
    }

    MidiMonitorView::MidiMonitorView (InstrumentHub& h, const Theme& t) : hub (h), theme (t)
    {
        instr::styleToggle (enableButton, theme.ok);
        enableButton.onClick = [this] { hub.setMidiMonitorEnabled (enableButton.getToggleState()); syncButtons(); };
        instr::styleToggle (pauseButton, theme.warn);
        pauseButton.onClick = [this] { syncButtons(); };
        clearButton.onClick = [this]
        {
            hub.clearMidi();
            rows.clear();
            visible.clear();
            countIn = countOut = 0;
            list.updateContent();
            repaint();
        };
        instr::styleToggle (clockButton, theme.accent);
        clockButton.setToggleState (true, juce::dontSendNotification);
        clockButton.setTooltip ("Hide MIDI clock and active sensing");
        clockButton.onClick = [this] { rebuildVisible(); };
        for (auto* b : { &enableButton, &pauseButton, &clearButton, &clockButton })
            addAndMakeVisible (b);

        list.setModel (this);
        list.setRowHeight (20);
        list.setColour (juce::ListBox::backgroundColourId, instr::style().screen);
        list.setColour (juce::ListBox::outlineColourId, instr::style().screenEdge);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        syncButtons();
        startTimerHz (15);
    }

    MidiMonitorView::~MidiMonitorView()
    {
        stopTimer();
        list.setModel (nullptr);
    }

    void MidiMonitorView::syncButtons()
    {
        enableButton.setToggleState (hub.isMidiMonitorEnabled(), juce::dontSendNotification);
        enableButton.setButtonText (hub.isMidiMonitorEnabled() ? "Recording" : "Enable");
        pauseButton.setButtonText (pauseButton.getToggleState() ? "Paused" : "Pause");
        repaint();
    }

    void MidiMonitorView::poll()
    {
        if (enableButton.getToggleState() != hub.isMidiMonitorEnabled())
            syncButtons();   // switched by the automation tool
        if (pauseButton.getToggleState())
            return;
        auto entries = hub.midiSince (cursor, 512);
        if (entries.empty())
            return;
        cursor = entries.back().index + 1;
        const bool hideClock = clockButton.getToggleState();
        for (auto& e : entries)
        {
            Row r { e, signal::describeMidi (e.bytes.data(), e.bytes.size()) };
            (e.toGuest ? countIn : countOut)++;
            rows.push_back (std::move (r));
            if (! (hideClock && (rows.back().text.kind == "clock" || rows.back().text.kind == "active_sensing")))
                visible.push_back ((int) rows.size() - 1);
        }
        if ((int) rows.size() > maxRows)
        {
            rows.erase (rows.begin(), rows.begin() + ((int) rows.size() - maxRows));
            rebuildVisible();
        }
        list.updateContent();
        list.scrollToEnsureRowIsOnscreen ((int) visible.size() - 1);
        repaint();
    }

    void MidiMonitorView::rebuildVisible()
    {
        visible.clear();
        const bool hideClock = clockButton.getToggleState();
        for (int i = 0; i < (int) rows.size(); ++i)
            if (! (hideClock && (rows[(size_t) i].text.kind == "clock" || rows[(size_t) i].text.kind == "active_sensing")))
                visible.push_back (i);
        list.updateContent();
        list.repaint();
    }

    void MidiMonitorView::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if (! juce::isPositiveAndBelow (row, (int) visible.size()))
            return;
        const auto& st = instr::style();
        const auto& r = rows[(size_t) visible[(size_t) row]];
        if (selected)
            g.fillAll (theme.accent.withAlpha (0.2f));
        else if (row % 2)
            g.fillAll (juce::Colours::white.withAlpha (0.025f));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (6, 0);
        g.setFont (theme.monoFont (11.5f));
        g.setColour (st.label);
        g.drawText (juce::String (r.entry.time, 3), area.removeFromLeft (64), juce::Justification::centredRight);
        area.removeFromLeft (8);
        g.setColour (r.entry.toGuest ? st.ch2 : st.ch1);
        g.drawText (r.entry.toGuest ? "IN" : "OUT", area.removeFromLeft (30), juce::Justification::centredLeft);
        g.setColour (st.label);
        g.drawText (r.text.channel > 0 ? juce::String (r.text.channel) : juce::String ("-"), area.removeFromLeft (22), juce::Justification::centred);
        area.removeFromLeft (6);

        juce::StringArray hex;
        for (size_t i = 0; i < r.entry.bytes.size() && i < 6; ++i)
            hex.add (juce::String::toHexString (r.entry.bytes[i]).paddedLeft ('0', 2).toUpperCase());
        auto hexText = hex.joinIntoString (" ") + (r.entry.bytes.size() > 6 ? " .." : "");
        const int hexWidth = juce::jmin (130, area.getWidth() / 3);
        g.drawText (hexText, area.removeFromRight (hexWidth), juce::Justification::centredRight);
        g.setColour (r.text.isRealtime ? st.label : st.readout);
        g.drawText (r.text.text, area, juce::Justification::centredLeft, true);
    }

    void MidiMonitorView::resized()
    {
        auto r = getLocalBounds().reduced (10, 8);
        auto row = r.removeFromTop (26);
        const int w = (row.getWidth() - 18) / 4;
        enableButton.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        pauseButton.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        clearButton.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (6);
        clockButton.setBounds (row);
        r.removeFromTop (6 + headerHeight);
        r.removeFromBottom (18);
        list.setBounds (r);
    }

    void MidiMonitorView::paint (juce::Graphics& g)
    {
        const auto& st = instr::style();
        g.fillAll (theme.chrome);
        auto r = getLocalBounds().reduced (10, 8);
        r.removeFromTop (26 + 6);
        auto header = r.removeFromTop (headerHeight).reduced (7, 0);
        g.setFont (theme.uiFont (11.0f, true));
        g.setColour (st.label);
        g.drawText ("TIME s", header.removeFromLeft (64), juce::Justification::centredRight);
        header.removeFromLeft (8);
        g.drawText ("DIR", header.removeFromLeft (30), juce::Justification::centredLeft);
        g.drawText ("CH", header.removeFromLeft (22), juce::Justification::centred);
        header.removeFromLeft (6);
        g.drawText ("BYTES", header.removeFromRight (juce::jmin (130, header.getWidth() / 3)), juce::Justification::centredRight);
        g.drawText ("MESSAGE", header, juce::Justification::centredLeft);

        auto footer = r.removeFromBottom (18);
        g.setFont (theme.monoFont (11.0f));
        g.setColour (hub.isMidiMonitorEnabled() ? st.label : theme.warn);
        juce::String text = hub.isMidiMonitorEnabled() ? juce::String() : juce::String ("Monitor off: press Enable.  ");
        text << "IN " << (juce::int64) countIn << "   OUT " << (juce::int64) countOut << "   shown " << (int) visible.size();
        g.drawText (text, footer, juce::Justification::centredLeft);
    }
}
