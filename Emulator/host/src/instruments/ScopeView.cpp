#include "instruments/ScopeView.h"

#include <cmath>

namespace kickemu
{
    namespace
    {
        /** A sample count or index from a time computation, clamped before the integer cast:
            a ring that never received data reports an enormous age, and the overflowed int
            once sized a vector negative (length_error, the app terminated). */
        int clampedCount (double value, int lo, int hi)
        {
            if (! std::isfinite (value))
                return value > 0 ? hi : lo;
            return (int) juce::jlimit ((double) lo, (double) juce::jmax (lo, hi), value);
        }

        constexpr int hDivs = 10, vDivs = 8;
        const double timebases[] = { 50e-6, 100e-6, 200e-6, 500e-6, 1e-3, 2e-3, 5e-3, 10e-3, 20e-3, 50e-3, 100e-3, 200e-3, 500e-3, 1.0 };
        const double audioScales[] = { 1.0, 0.5, 0.25, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.001 };
        const double cvScales[] = { 2.0, 1.0, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01 };

        juce::String timeText (double s) { return instr::eng (s, "s", 2).replace (" ", ""); }

        template <size_t N>
        int nearestIndex (const double (&values)[N], double v)
        {
            int best = 0;
            for (int i = 1; i < (int) N; ++i)
                if (std::abs (std::log (values[i] / v)) < std::abs (std::log (values[best] / v)))
                    best = i;
            return best;
        }
    }

    ScopeView::ScopeView (InstrumentHub& h, const Theme& t) : hub (h), theme (t)
    {
        const auto& st = instr::style();
        for (int c = 0; c < 2; ++c)
        {
            chLabel[c].setText ("CH" + juce::String (c + 1), juce::dontSendNotification);
            chLabel[c].setFont (theme.uiFont (12.5f, true));
            chLabel[c].setColour (juce::Label::textColourId, c == 0 ? st.ch1 : st.ch2);
            addAndMakeVisible (chLabel[c]);
            fillSourceBox (sourceBox[c]);
            sourceBox[c].onChange = [this] { readControls(); };
            scaleBox[c].onChange = [this] { readControls(); };
            addAndMakeVisible (sourceBox[c]);
            addAndMakeVisible (scaleBox[c]);
        }
        for (int i = 0; i < (int) std::size (timebases); ++i)
            timeBox.addItem (timeText (timebases[i]) + "/div", i + 1);
        timeBox.onChange = [this] { readControls(); };
        addAndMakeVisible (timeBox);

        trigSourceBox.addItem ("Trig CH1", 1);
        trigSourceBox.addItem ("Trig CH2", 2);
        trigSourceBox.onChange = [this] { readControls(); };
        addAndMakeVisible (trigSourceBox);

        edgeButton.onClick = [this] { settings.risingEdge = ! settings.risingEdge; syncControls(); };
        edgeButton.setTooltip ("Trigger edge");
        addAndMakeVisible (edgeButton);
        modeButton.onClick = [this] { settings.autoMode = ! settings.autoMode; syncControls(); };
        modeButton.setTooltip ("Auto: free-runs without a trigger. Normal: waits for one.");
        addAndMakeVisible (modeButton);
        instr::styleToggle (runButton, theme.ok);
        runButton.onClick = [this] { settings.running = runButton.getToggleState(); syncControls(); };
        addAndMakeVisible (runButton);

        levelLabel.setFont (theme.uiFont (12.0f));
        levelLabel.setColour (juce::Label::textColourId, theme.chromeDim);
        addAndMakeVisible (levelLabel);
        levelSlider.setColour (juce::Slider::trackColourId, st.trig.withAlpha (0.6f));
        levelSlider.setColour (juce::Slider::thumbColourId, st.trig);
        levelSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 84, 20);
        levelSlider.onValueChange = [this] { if (! updating) settings.triggerLevel = levelSlider.getValue(); };
        addAndMakeVisible (levelSlider);

        syncControls();
        startTimerHz (30);
    }

    ScopeView::~ScopeView() { stopTimer(); }

    void ScopeView::fillSourceBox (juce::ComboBox& box)
    {
        box.addItem ("Off", 1);
        int id = 2;
        for (auto& s : hub.getSources())
            box.addItem (s.label, id++);
    }

    void ScopeView::fillScaleBox (juce::ComboBox& box, bool audio, double current)
    {
        box.clear (juce::dontSendNotification);
        if (audio)
            for (int i = 0; i < (int) std::size (audioScales); ++i)
                box.addItem (juce::String (audioScales[i]) + " FS/div", i + 1);
        else
            for (int i = 0; i < (int) std::size (cvScales); ++i)
                box.addItem (instr::eng (cvScales[i], "V", 2).replace (" ", "") + "/div", i + 1);
        box.setSelectedId (audio ? nearestIndex (audioScales, current) + 1 : nearestIndex (cvScales, current) + 1, juce::dontSendNotification);
    }

    void ScopeView::setSettings (const Settings& s)
    {
        settings = s;
        syncControls();
    }

    void ScopeView::syncControls()
    {
        juce::ScopedValueSetter<bool> guard (updating, true);
        auto sources = hub.getSources();
        for (int c = 0; c < 2; ++c)
        {
            int id = 1;
            bool audio = true;
            for (int i = 0; i < (int) sources.size(); ++i)
                if (sources[(size_t) i].id == settings.ch[c].source)
                {
                    id = i + 2;
                    audio = sources[(size_t) i].audio;
                }
            sourceBox[c].setSelectedId (id, juce::dontSendNotification);
            fillScaleBox (scaleBox[c], audio, settings.ch[c].perDiv);
            scaleBox[c].setEnabled (id != 1);
        }
        timeBox.setSelectedId (nearestIndex (timebases, settings.secPerDiv) + 1, juce::dontSendNotification);
        trigSourceBox.setSelectedId (settings.triggerChannel + 1, juce::dontSendNotification);
        edgeButton.setButtonText (settings.risingEdge ? "Rise" : "Fall");
        modeButton.setButtonText (settings.autoMode ? "Auto" : "Normal");
        runButton.setToggleState (settings.running, juce::dontSendNotification);
        runButton.setButtonText (settings.running ? "Run" : "Stop");

        InstrumentHub::Source ts;
        const bool haveTrig = hub.findSource (settings.ch[settings.triggerChannel].source, ts);
        const double range = haveTrig ? (ts.audio ? 1.0 : ts.fullScale) : 1.0;
        levelSlider.setRange (haveTrig && ! ts.audio ? 0.0 : -range, range, range / 1000.0);
        levelSlider.setTextValueSuffix (haveTrig && ! ts.audio ? " V" : " FS");
        levelSlider.setNumDecimalPlacesToDisplay (3);
        levelSlider.setValue (settings.triggerLevel, juce::dontSendNotification);
        repaint();
    }

    void ScopeView::readControls()
    {
        if (updating)
            return;
        auto sources = hub.getSources();
        for (int c = 0; c < 2; ++c)
        {
            const int id = sourceBox[c].getSelectedId();
            const auto previous = settings.ch[c].source;
            settings.ch[c].source = id >= 2 && id - 2 < (int) sources.size() ? sources[(size_t) id - 2].id : juce::String();
            InstrumentHub::Source s;
            const bool known = hub.findSource (settings.ch[c].source, s);
            const int sel = scaleBox[c].getSelectedId() - 1;
            if (previous != settings.ch[c].source && known)
                settings.ch[c].perDiv = s.audio ? 0.25 : 1.0;   // sensible default per kind
            else if (known && sel >= 0)
                settings.ch[c].perDiv = s.audio ? audioScales[juce::jmin (sel, (int) std::size (audioScales) - 1)]
                                                : cvScales[juce::jmin (sel, (int) std::size (cvScales) - 1)];
        }
        if (auto i = timeBox.getSelectedId() - 1; i >= 0)
            settings.secPerDiv = timebases[i];
        const int trig = trigSourceBox.getSelectedId() - 1;
        if (trig >= 0 && trig != settings.triggerChannel)
        {
            settings.triggerChannel = trig;
            settings.triggerLevel = 0.0;
        }
        syncControls();
    }

    // ------------------------------------------------------------------------------------
    // acquisition

    void ScopeView::acquire()
    {
        if (! settings.running)
        {
            status = "STOP";
            repaint();
            return;
        }

        int tc = settings.triggerChannel;
        InstrumentHub::Source src[2];
        bool active[2];
        for (int c = 0; c < 2; ++c)
            active[c] = hub.findSource (settings.ch[c].source, src[c]);
        if (! active[tc])
            tc = 1 - tc;
        if (! active[tc])
        {
            traces[0].active = traces[1].active = false;
            status = "NO CHANNEL";
            repaint();
            return;
        }

        const double screenSec = hDivs * settings.secPerDiv;

        // Trigger channel: the newest two screens, searched backwards for a crossing that
        // leaves half a screen on either side.
        auto& ring = hub.ringFor (src[tc]);
        const double rate = ring.getSampleRate();
        const int nScreen = clampedCount (std::ceil (screenSec * rate), 2, ring.getCapacity());
        const int nRead = juce::jmin (ring.getCapacity(), 2 * nScreen + 4);
        std::vector<float> buf ((size_t) nRead);
        juce::int64 end = 0;
        const int count = ring.read (src[tc].channel, nRead, buf.data(), -1, end);
        buf.resize ((size_t) count);
        const double ageMs = ring.msSinceLastWrite();
        const double half = nScreen / 2.0;
        const double level = settings.triggerLevel;

        double centre = -1.0;
        for (int i = count - (int) std::ceil (half); i >= (int) std::ceil (half) + 1; --i)
        {
            const double a = buf[(size_t) i - 1], b = buf[(size_t) i];
            const bool hit = settings.risingEdge ? (a < level && b >= level) : (a > level && b <= level);
            if (hit)
            {
                centre = (i - 1) + (std::abs (b - a) > 0.0 ? (level - a) / (b - a) : 0.0);
                break;
            }
        }
        if (centre < 0.0)
        {
            if (! settings.autoMode)
            {
                status = "WAIT";
                repaint();
                return;
            }
            centre = count - half;
            status = "AUTO";
        }
        else
        {
            status = "TRIG'D";
        }

        auto finish = [&] (Trace& t, const InstrumentHub::Source& s, std::vector<float> samples, double centrePos, double r, double age)
        {
            t.active = true;
            t.source = s;
            t.samples = std::move (samples);
            t.samplesPerDiv = settings.secPerDiv * r;
            t.leftPos = centrePos - hDivs * t.samplesPerDiv / 2.0;
            t.stale = age > 1000.0;
            const int from = clampedCount (std::floor (t.leftPos), 0, (int) t.samples.size());
            const int to = clampedCount (std::ceil (t.leftPos + hDivs * t.samplesPerDiv), from, (int) t.samples.size());
            t.m = signal::measure (t.samples.data() + from, to - from, r);
        };

        const double centreFromEnd = count - centre;   // samples before the newest
        finish (traces[tc], src[tc], buf, centre, rate, ageMs);

        const int oc = 1 - tc;
        if (! active[oc])
        {
            traces[oc].active = false;
        }
        else if (src[oc].audio == src[tc].audio)
        {
            std::vector<float> other ((size_t) count);
            juce::int64 used = 0;
            const int got = ring.read (src[oc].channel, count, other.data(), end, used);
            other.resize ((size_t) got);
            finish (traces[oc], src[oc], std::move (other), centre - (count - got), rate, ageMs);
        }
        else
        {
            // Different ring: line the two up by arrival time on the host.
            auto& oring = hub.ringFor (src[oc]);
            const double orate = oring.getSampleRate();
            const double oage = oring.msSinceLastWrite();
            const double centreAgoSec = centreFromEnd / rate + ageMs * 0.001;
            const double fromEnd = juce::jmax (0.0, (centreAgoSec - oage * 0.001) * orate);
            const int oScreen = clampedCount (std::ceil (screenSec * orate), 2, oring.getCapacity());
            const int oRead = clampedCount (std::ceil (fromEnd + oScreen / 2.0) + 4.0, 0, oring.getCapacity());
            std::vector<float> other ((size_t) oRead);
            juce::int64 used = 0;
            const int got = oring.read (src[oc].channel, oRead, other.data(), -1, used);
            other.resize ((size_t) got);
            finish (traces[oc], src[oc], std::move (other), got - fromEnd, orate, oage);
        }
        repaint();
    }

    // ------------------------------------------------------------------------------------
    // layout and drawing

    void ScopeView::resized()
    {
        auto r = getLocalBounds().reduced (10, 8);
        for (int c = 0; c < 2; ++c)
        {
            auto row = r.removeFromTop (26);
            chLabel[c].setBounds (row.removeFromLeft (40));
            scaleBox[c].setBounds (row.removeFromRight (112));
            row.removeFromRight (6);
            sourceBox[c].setBounds (row);
            r.removeFromTop (4);
        }
        {
            auto row = r.removeFromTop (26);
            const int w = row.getWidth();
            timeBox.setBounds (row.removeFromLeft (juce::jmax (96, w * 26 / 100)));
            row.removeFromLeft (6);
            trigSourceBox.setBounds (row.removeFromLeft (juce::jmax (84, w * 22 / 100)));
            row.removeFromLeft (6);
            const int bw = (row.getWidth() - 12) / 3;
            edgeButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (6);
            modeButton.setBounds (row.removeFromLeft (bw));
            row.removeFromLeft (6);
            runButton.setBounds (row);
            r.removeFromTop (4);
        }
        {
            auto row = r.removeFromTop (24);
            levelLabel.setBounds (row.removeFromLeft (40));
            levelSlider.setBounds (row);
        }
    }

    juce::Rectangle<float> ScopeView::screenArea() const
    {
        auto r = getLocalBounds().reduced (10, 8).toFloat();
        r.removeFromTop (26 * 3 + 4 * 3 + 24 + 8);
        r.removeFromBottom (44);
        return r;
    }

    void ScopeView::drawGrid (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        const auto& st = instr::style();
        const float dx = r.getWidth() / hDivs, dy = r.getHeight() / vDivs;
        g.setColour (st.grid);
        for (int i = 1; i < hDivs; ++i)
            g.drawVerticalLine (juce::roundToInt (r.getX() + (float) i * dx), r.getY(), r.getBottom());
        for (int i = 1; i < vDivs; ++i)
            g.drawHorizontalLine (juce::roundToInt (r.getY() + (float) i * dy), r.getX(), r.getRight());
        // Centre axes with ticks every fifth of a division
        g.setColour (st.axis);
        const float cx = r.getCentreX(), cy = r.getCentreY();
        g.drawHorizontalLine (juce::roundToInt (cy), r.getX(), r.getRight());
        g.drawVerticalLine (juce::roundToInt (cx), r.getY(), r.getBottom());
        for (int i = 0; i < hDivs * 5; ++i)
            g.fillRect (r.getX() + (float) i * dx / 5.0f, cy - 2.0f, 1.0f, 4.0f);
        for (int i = 0; i < vDivs * 5; ++i)
            g.fillRect (cx - 2.0f, r.getY() + (float) i * dy / 5.0f, 4.0f, 1.0f);
    }

    void ScopeView::drawTrace (juce::Graphics& g, juce::Rectangle<float> r, const Trace& t, double perDiv, double zeroDiv, juce::Colour c) const
    {
        if (! t.active || t.samples.empty() || perDiv <= 0.0)
            return;
        const double screenSamples = hDivs * t.samplesPerDiv;
        const double pxPerSample = r.getWidth() / screenSamples;
        const float dy = r.getHeight() / vDivs;
        auto yOf = [&] (double v) { return (float) (r.getCentreY() - (v / perDiv + zeroDiv) * dy); };
        auto xOf = [&] (double k) { return (float) (r.getX() + (k - t.leftPos) * pxPerSample); };

        const int n = (int) t.samples.size();
        const int first = clampedCount (std::floor (t.leftPos), 0, n - 1);
        const int last = clampedCount (std::ceil (t.leftPos + screenSamples), 0, n - 1);
        juce::Path p;
        if (pxPerSample >= 0.5)
        {
            for (int k = first; k <= last; ++k)
            {
                const juce::Point<float> pt (xOf (k), yOf (t.samples[(size_t) k]));
                if (k == first) p.startNewSubPath (pt); else p.lineTo (pt);
            }
        }
        else
        {
            // Many samples per pixel: a min/max envelope per column keeps peaks visible.
            const int columns = juce::roundToInt (r.getWidth());
            bool started = false;
            for (int col = 0; col < columns; ++col)
            {
                const int a = clampedCount (std::floor (t.leftPos + col / pxPerSample), first, last);
                const int b = clampedCount (std::floor (t.leftPos + (col + 1) / pxPerSample), a, last);
                float lo = t.samples[(size_t) a], hi = lo;
                for (int k = a + 1; k <= b; ++k)
                {
                    lo = juce::jmin (lo, t.samples[(size_t) k]);
                    hi = juce::jmax (hi, t.samples[(size_t) k]);
                }
                const float x = r.getX() + (float) col + 0.5f;
                if (! started) { p.startNewSubPath (x, yOf (lo)); started = true; }
                else p.lineTo (x, yOf (lo));
                p.lineTo (x, yOf (hi));
            }
        }
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r.toNearestInt());
        instr::strokeTrace (g, p, t.stale ? c.withMultipliedSaturation (0.3f).withMultipliedBrightness (0.6f) : c);

        // Ground marker on the left edge
        const float y0 = juce::jlimit (r.getY() + 4.0f, r.getBottom() - 4.0f, yOf (0.0));
        juce::Path m;
        m.addTriangle (r.getX(), y0 - 4.0f, r.getX(), y0 + 4.0f, r.getX() + 6.0f, y0);
        g.setColour (c);
        g.fillPath (m);
    }

    void ScopeView::paint (juce::Graphics& g)
    {
        const auto& st = instr::style();
        g.fillAll (theme.chrome);
        auto outer = screenArea();
        auto r = instr::drawScreen (g, outer).reduced (6.0f);
        drawGrid (g, r);

        double perDiv[2], zero[2];
        for (int c = 0; c < 2; ++c)
        {
            perDiv[c] = settings.ch[c].perDiv;
            zero[c] = traces[c].active ? zeroDivFor (traces[c].source) : 0.0;
        }
        drawTrace (g, r, traces[1], perDiv[1], zero[1], st.ch2);
        drawTrace (g, r, traces[0], perDiv[0], zero[0], st.ch1);

        // Trigger level and position markers
        const int tc = settings.triggerChannel;
        InstrumentHub::Source ts;
        if (hub.findSource (settings.ch[tc].source, ts))
        {
            const float dy = r.getHeight() / vDivs;
            const float y = (float) (r.getCentreY() - (settings.triggerLevel / settings.ch[tc].perDiv + zeroDivFor (ts)) * dy);
            if (y >= r.getY() && y <= r.getBottom())
            {
                juce::Path m;
                m.addTriangle (r.getRight(), y - 4.0f, r.getRight(), y + 4.0f, r.getRight() - 6.0f, y);
                g.setColour (st.trig);
                g.fillPath (m);
                g.setColour (st.trig.withAlpha (0.25f));
                g.drawHorizontalLine (juce::roundToInt (y), r.getRight() - 30.0f, r.getRight() - 6.0f);
            }
            juce::Path t;
            t.addTriangle (r.getCentreX() - 4.0f, r.getY(), r.getCentreX() + 4.0f, r.getY(), r.getCentreX(), r.getY() + 6.0f);
            g.setColour (st.trig);
            g.fillPath (t);
        }

        // Status (top right) and per-channel stale flags
        g.setFont (theme.monoFont (11.0f));
        const auto statusColour = status == "TRIG'D" ? theme.ok : status == "STOP" ? theme.bad : theme.warn;
        g.setColour (statusColour);
        g.drawText (status, r.reduced (6.0f, 4.0f), juce::Justification::topRight);
        for (int c = 0; c < 2; ++c)
            if (traces[c].active && traces[c].stale)
            {
                g.setColour ((c == 0 ? st.ch1 : st.ch2).withAlpha (0.8f));
                g.drawText ("CH" + juce::String (c + 1) + " NO SIGNAL", r.reduced (6.0f, 4.0f).withTrimmedTop ((float) c * 14.0f),
                            juce::Justification::topLeft);
            }

        // Readout strip under the screen: scale and measurements per channel, timebase.
        auto strip = getLocalBounds().reduced (10, 8).toFloat().withTop (outer.getBottom() + 4.0f);
        g.setFont (theme.monoFont (11.5f));
        for (int c = 0; c < 2; ++c)
        {
            auto line = strip.removeFromTop (16.0f);
            const auto& t = traces[c];
            const auto colour = c == 0 ? st.ch1 : st.ch2;
            juce::String text = "CH" + juce::String (c + 1) + " ";
            if (! t.active)
                text << "off";
            else
            {
                const auto unit = t.source.audio ? juce::String ("FS") : juce::String ("V");
                text << (t.source.audio ? juce::String (settings.ch[c].perDiv) + "FS" : instr::eng (settings.ch[c].perDiv, "V", 2).replace (" ", "")) << "/div  "
                     << "pp " << instr::eng (t.m.max - t.m.min, unit, 3) << "  rms " << instr::eng (t.m.rms, unit, 3) << "  "
                     << (t.m.frequency > 0.0 ? instr::eng (t.m.frequency, "Hz", 4) : juce::String ("--- Hz"));
            }
            g.setColour (colour);
            g.drawText (text, line, juce::Justification::centredLeft);
        }
        g.setColour (st.label);
        auto line = strip.removeFromTop (16.0f);
        juce::String tb = timeText (settings.secPerDiv) + "/div   trig CH" + juce::String (tc + 1) + (settings.risingEdge ? " rise " : " fall ");
        tb << (hub.findSource (settings.ch[tc].source, ts) ? (ts.audio ? juce::String (settings.triggerLevel, 3) + " FS" : juce::String (settings.triggerLevel, 3) + " V") : juce::String ("-"));
        g.drawText (tb, line, juce::Justification::centredLeft);
    }
}
