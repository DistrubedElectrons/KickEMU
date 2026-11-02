#include "instruments/MeterView.h"

#include <cmath>

namespace kickemu
{
    namespace
    {
        const double windows[] = { 0.1, 0.25, 1.0 };
        constexpr int controlsHeight = 26;
    }

    MeterView::MeterView (InstrumentHub& h, const Theme& t) : hub (h), theme (t)
    {
        for (int i = 0; i < 2; ++i)
        {
            int id = 1;
            for (auto& s : hub.getSources())
                sourceBox[i].addItem (s.label, id++);
            for (int w = 0; w < (int) std::size (windows); ++w)
                windowBox[i].addItem (instr::eng (windows[w], "s", 3).replace (".00 ", " ").replace (" ", "") + " window", w + 1);
            sourceBox[i].onChange = [this, i]
            {
                if (updating) return;
                auto list = hub.getSources();
                const int k = sourceBox[i].getSelectedId() - 1;
                if (k >= 0 && k < (int) list.size())
                    inputs[i].source = list[(size_t) k].id;
                acquire();
            };
            windowBox[i].onChange = [this, i]
            {
                if (updating) return;
                inputs[i].windowSec = windows[juce::jlimit (0, (int) std::size (windows) - 1, windowBox[i].getSelectedId() - 1)];
                acquire();
            };
            addAndMakeVisible (sourceBox[i]);
            addAndMakeVisible (windowBox[i]);
            sync (i);
        }
        startTimerHz (8);
    }

    MeterView::~MeterView() { stopTimer(); }

    void MeterView::setInput (int index, const Input& input)
    {
        inputs[index] = input;
        sync (index);
        acquire();
    }

    void MeterView::sync (int i)
    {
        juce::ScopedValueSetter<bool> guard (updating, true);
        auto list = hub.getSources();
        for (int k = 0; k < (int) list.size(); ++k)
            if (list[(size_t) k].id == inputs[i].source)
                sourceBox[i].setSelectedId (k + 1, juce::dontSendNotification);
        for (int w = 0; w < (int) std::size (windows); ++w)
            if (std::abs (windows[w] - inputs[i].windowSec) < 1e-9)
                windowBox[i].setSelectedId (w + 1, juce::dontSendNotification);
    }

    void MeterView::acquire()
    {
        for (int i = 0; i < 2; ++i)
        {
            InstrumentHub::Capture cap;
            if (! hub.findSource (inputs[i].source, sources[i]) || ! hub.capture (inputs[i].source, inputs[i].windowSec, cap))
                continue;
            readings[i] = signal::measure (cap.samples.data(), (int) cap.samples.size(), cap.sampleRate);
            stale[i] = cap.ageMs > 1000.0;
        }
        repaint();
    }

    void MeterView::resized()
    {
        for (int i = 0; i < 2; ++i)
        {
            auto r = panelArea (i).toNearestInt().reduced (10, 8);
            auto row = r.removeFromTop (controlsHeight);
            windowBox[i].setBounds (row.removeFromRight (juce::jmax (110, row.getWidth() * 34 / 100)));
            row.removeFromRight (6);
            sourceBox[i].setBounds (row.withTrimmedLeft (28));
        }
    }

    juce::Rectangle<float> MeterView::panelArea (int i) const
    {
        auto r = getLocalBounds().reduced (10, 8).toFloat();
        const float h = (r.getHeight() - 10.0f) / 2.0f;
        return i == 0 ? r.removeFromTop (h) : r.withTrimmedTop (h + 10.0f);
    }

    void MeterView::paint (juce::Graphics& g)
    {
        g.fillAll (theme.chrome);
        for (int i = 0; i < 2; ++i)
            paintPanel (g, i, panelArea (i));
    }

    void MeterView::paintPanel (juce::Graphics& g, int i, juce::Rectangle<float> area) const
    {
        const auto& st = instr::style();
        const auto colour = i == 0 ? st.ch1 : st.ch2;
        auto r = instr::drawScreen (g, area).reduced (10.0f, 8.0f);

        // Input badge next to the source selector
        auto head = r.removeFromTop ((float) controlsHeight);
        g.setColour (colour);
        g.setFont (theme.uiFont (13.0f, true));
        g.drawText (i == 0 ? "A" : "B", head.removeFromLeft (22.0f), juce::Justification::centred);
        r.removeFromTop (6.0f);

        const auto& s = sources[i];
        const auto& m = readings[i];
        const bool audio = s.audio;
        const bool none = m.samples == 0;

        // Primary reading
        juce::String big, bigUnit, caption;
        if (none)
        {
            big = "----";
        }
        else if (audio)
        {
            const double db = signal::toDb (m.rms);
            big = db <= -150.0 ? juce::String ("-inf") : juce::String (db, 1);
            bigUnit = "dBFS";
            caption = "RMS";
        }
        else
        {
            big = juce::String (m.mean, 3);
            bigUnit = "V";
            caption = "DC";
        }
        auto bigArea = r.removeFromTop (juce::jmin (64.0f, r.getHeight() * 0.36f));
        g.setColour (st.label);
        g.setFont (theme.uiFont (11.0f, true));
        g.drawText (caption, bigArea.removeFromLeft (40.0f).withTrimmedTop (6.0f), juce::Justification::topLeft);
        const float bigSize = juce::jlimit (24.0f, 54.0f, bigArea.getHeight() * 0.95f);
        auto unitArea = bigArea.removeFromRight (bigSize * 1.6f);
        g.setColour (stale[i] ? colour.withMultipliedSaturation (0.3f).withMultipliedBrightness (0.6f) : colour);
        g.setFont (theme.monoFont (bigSize));
        g.drawText (big, bigArea, juce::Justification::centredRight);
        g.setFont (theme.uiFont (bigSize * 0.38f, true));
        g.drawText (bigUnit, unitArea.withTrimmedLeft (8.0f), juce::Justification::centredLeft);

        // Bar: -60..0 dBFS (peak tick) for audio, 0..full scale for CV
        auto bar = r.removeFromTop (10.0f).reduced (0.0f, 1.0f);
        g.setColour (st.grid);
        g.fillRoundedRectangle (bar, 2.0f);
        if (! none)
        {
            auto frac = [&] (double v)
            {
                return audio ? juce::jlimit (0.0, 1.0, (signal::toDb (v) + 60.0) / 60.0) : juce::jlimit (0.0, 1.0, v / juce::jmax (0.1, s.fullScale));
            };
            const double level = frac (audio ? m.rms : m.mean);
            g.setColour (colour.withAlpha (0.85f));
            g.fillRoundedRectangle (bar.withWidth ((float) (bar.getWidth() * level)), 2.0f);
            const float px = bar.getX() + (float) (bar.getWidth() * frac (audio ? m.peak : m.max));
            g.setColour (audio && m.peak >= 0.999 ? theme.bad : juce::Colours::white.withAlpha (0.8f));
            g.fillRect (px - 1.0f, bar.getY() - 1.0f, 2.0f, bar.getHeight() + 2.0f);
        }
        r.removeFromTop (8.0f);

        // Secondary readings, two columns
        const auto unit = audio ? juce::String ("FS") : juce::String ("V");
        auto num = [&] (double v) { return none ? juce::String ("---") : instr::eng (v, unit, 4); };
        juce::StringArray names, values;
        names.add ("DC");        values.add (num (m.mean));
        names.add ("RMS");       values.add (num (m.rms));
        names.add ("AC RMS");    values.add (num (m.acRms));
        names.add ("Peak");      values.add (none ? "---" : audio ? juce::String (signal::toDb (m.peak), 1) + " dBFS" : num (m.peak));
        names.add ("Min / Max"); values.add (none ? "---" : instr::eng (m.min, unit, 3) + " / " + instr::eng (m.max, unit, 3));
        names.add ("Freq");      values.add (none || m.frequency <= 0.0 ? juce::String ("---") : instr::eng (m.frequency, "Hz", 5));
        const int rows = (names.size() + 1) / 2;
        const float rowH = juce::jlimit (14.0f, 22.0f, r.getHeight() / (float) rows);
        for (int k = 0; k < names.size(); ++k)
        {
            auto cell = juce::Rectangle<float> (r.getX() + (float) (k % 2) * r.getWidth() / 2.0f, r.getY() + (float) (k / 2) * rowH, r.getWidth() / 2.0f - 6.0f, rowH);
            g.setColour (st.label);
            g.setFont (theme.uiFont (11.0f));
            g.drawText (names[k], cell, juce::Justification::centredLeft);
            g.setColour (st.readout);
            g.setFont (theme.monoFont (12.5f));
            g.drawText (values[k], cell, juce::Justification::centredRight);
        }
        if (stale[i])
        {
            g.setColour (theme.warn);
            g.setFont (theme.monoFont (10.5f));
            g.drawText ("NO SIGNAL", area.reduced (12.0f, 6.0f), juce::Justification::bottomRight);
        }
    }
}
