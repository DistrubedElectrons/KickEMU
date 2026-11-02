#include "instruments/SpectrumView.h"

#include <cmath>

namespace kickemu
{
    namespace
    {
        constexpr double dbTop = 0.0, dbBottom = -140.0;
        const int fftSizes[] = { 1024, 2048, 4096, 8192, 16384, 32768 };
        const int averages[] = { 1, 4, 16, 64 };
    }

    SpectrumView::SpectrumView (InstrumentHub& h, const Theme& t) : hub (h), theme (t)
    {
        int id = 1;
        for (auto& s : hub.getSources())
            sourceBox.addItem (s.label, id++);
        sourceBox.onChange = [this]
        {
            if (updating) return;
            auto sources = hub.getSources();
            const int i = sourceBox.getSelectedId() - 1;
            if (i >= 0 && i < (int) sources.size())
                settings.source = sources[(size_t) i].id;
            setSettings (settings);
        };
        for (int i = 0; i < (int) std::size (fftSizes); ++i)
            sizeBox.addItem ("FFT " + juce::String (fftSizes[i]), i + 1);
        sizeBox.onChange = [this]
        {
            if (updating) return;
            settings.fftSize = fftSizes[juce::jlimit (0, (int) std::size (fftSizes) - 1, sizeBox.getSelectedId() - 1)];
            setSettings (settings);
        };
        for (int i = 0; i < (int) std::size (averages); ++i)
            avgBox.addItem (averages[i] == 1 ? juce::String ("Avg off") : "Avg " + juce::String (averages[i]), i + 1);
        avgBox.onChange = [this]
        {
            if (updating) return;
            settings.average = averages[juce::jlimit (0, (int) std::size (averages) - 1, avgBox.getSelectedId() - 1)];
            framesAveraged = 0;
        };
        instr::styleToggle (holdButton, instr::style().hold);
        holdButton.onClick = [this] { settings.peakHold = holdButton.getToggleState(); holdDb.clear(); repaint(); };
        instr::styleToggle (runButton, theme.ok);
        runButton.onClick = [this] { settings.running = runButton.getToggleState(); syncControls(); };
        for (auto* c : std::initializer_list<juce::Component*> { &sourceBox, &sizeBox, &avgBox, &holdButton, &runButton })
            addAndMakeVisible (c);
        syncControls();
        startTimerHz (20);
    }

    SpectrumView::~SpectrumView() { stopTimer(); }

    void SpectrumView::setSettings (const Settings& s)
    {
        settings = s;
        avgPower.clear();
        holdDb.clear();
        peaks.clear();
        framesAveraged = 0;
        lastEnd = -1;
        syncControls();
    }

    void SpectrumView::syncControls()
    {
        juce::ScopedValueSetter<bool> guard (updating, true);
        auto sources = hub.getSources();
        for (int i = 0; i < (int) sources.size(); ++i)
            if (sources[(size_t) i].id == settings.source)
                sourceBox.setSelectedId (i + 1, juce::dontSendNotification);
        for (int i = 0; i < (int) std::size (fftSizes); ++i)
            if (fftSizes[i] == settings.fftSize)
                sizeBox.setSelectedId (i + 1, juce::dontSendNotification);
        for (int i = 0; i < (int) std::size (averages); ++i)
            if (averages[i] == settings.average)
                avgBox.setSelectedId (i + 1, juce::dontSendNotification);
        holdButton.setToggleState (settings.peakHold, juce::dontSendNotification);
        runButton.setToggleState (settings.running, juce::dontSendNotification);
        runButton.setButtonText (settings.running ? "Run" : "Stop");
        repaint();
    }

    void SpectrumView::acquire()
    {
        if (! settings.running)
            return;
        InstrumentHub::Source src;
        if (! hub.findSource (settings.source, src))
            return;
        auto& ring = hub.ringFor (src);
        rate = ring.getSampleRate();
        const int n = juce::jmin (settings.fftSize, ring.getCapacity());
        std::vector<float> buf ((size_t) n);
        juce::int64 end = 0;
        const int got = ring.read (src.channel, n, buf.data(), -1, end);
        stale = ring.msSinceLastWrite() > 1000.0;
        if (got < 16 || end == lastEnd)
            return;
        lastEnd = end;

        auto p = signal::powerSpectrum (buf.data(), got, settings.fftSize);
        if ((int) avgPower.size() != (int) p.size() || analysedSize != settings.fftSize)
        {
            avgPower = p;
            holdDb.clear();
            framesAveraged = 1;
            analysedSize = settings.fftSize;
        }
        else
        {
            framesAveraged = juce::jmin (framesAveraged + 1, settings.average);
            const float k = 1.0f / (float) framesAveraged;
            for (size_t i = 0; i < p.size(); ++i)
                avgPower[i] += (p[i] - avgPower[i]) * k;
        }

        signal::Spectrum s;
        s.sampleRate = rate;
        s.fftSize = settings.fftSize;
        s.db.resize (avgPower.size());
        for (size_t i = 0; i < avgPower.size(); ++i)
            s.db[i] = (float) (avgPower[i] > 1.0e-20f ? 10.0 * std::log10 (avgPower[i]) : -200.0);
        if (settings.peakHold)
        {
            if (holdDb.size() != s.db.size())
                holdDb = s.db;
            for (size_t i = 0; i < s.db.size(); ++i)
                holdDb[i] = juce::jmax (holdDb[i], s.db[i]);
        }
        peaks = signal::findPeaks (s, 3, -120.0);
        repaint();
    }

    void SpectrumView::resized()
    {
        auto r = getLocalBounds().reduced (10, 8);
        auto row = r.removeFromTop (26);
        sourceBox.setBounds (row.removeFromLeft (row.getWidth() * 34 / 100));
        row.removeFromLeft (6);
        sizeBox.setBounds (row.removeFromLeft (row.getWidth() * 30 / 100));
        row.removeFromLeft (6);
        avgBox.setBounds (row.removeFromLeft (row.getWidth() * 34 / 100));
        row.removeFromLeft (6);
        runButton.setBounds (row.removeFromRight (row.getWidth() * 40 / 100));
        row.removeFromRight (6);
        holdButton.setBounds (row);
    }

    juce::Rectangle<float> SpectrumView::graphArea() const
    {
        auto r = getLocalBounds().reduced (10, 8).toFloat();
        r.removeFromTop (26 + 8);
        r.removeFromBottom (56);
        return r;
    }

    double SpectrumView::fMin() const
    {
        return juce::jmax (10.0, rate / juce::jmax (1, settings.fftSize));
    }

    float SpectrumView::xOf (double hz, juce::Rectangle<float> r) const
    {
        const double lo = std::log (fMin()), hi = std::log (rate / 2.0);
        return (float) (r.getX() + (std::log (juce::jmax (hz, 1.0e-3)) - lo) / (hi - lo) * r.getWidth());
    }

    void SpectrumView::paint (juce::Graphics& g)
    {
        const auto& st = instr::style();
        g.fillAll (theme.chrome);
        auto outer = graphArea();
        auto plot = instr::drawScreen (g, outer).reduced (8.0f, 6.0f);
        plot.removeFromLeft (30.0f);    // dB labels
        plot.removeFromBottom (14.0f);  // Hz labels

        auto yOf = [&] (double db) { return (float) (plot.getY() + (dbTop - db) / (dbTop - dbBottom) * plot.getHeight()); };

        // Grid: decades and 2/5 steps, every 20 dB
        g.setFont (theme.monoFont (10.0f));
        for (double db = dbTop; db >= dbBottom; db -= 20.0)
        {
            const float y = yOf (db);
            g.setColour (db > dbTop - 1.0 ? st.axis : st.grid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (st.label);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 32.0f, y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight);
        }
        for (double decade = 1.0; decade < rate; decade *= 10.0)
            for (double m : { 1.0, 2.0, 5.0 })
            {
                const double f = decade * m;
                if (f < fMin() || f > rate / 2.0)
                    continue;
                const float x = xOf (f, plot);
                g.setColour (m < 1.5 ? st.axis : st.grid);
                g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
                g.setColour (st.label);
                const auto label = f >= 1000.0 ? juce::String (f / 1000.0) + "k" : juce::String ((int) f);
                g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 13.0f), juce::Justification::centred);
            }

        auto curve = [&] (const std::vector<float>& db, bool isPower)
        {
            juce::Path p;
            if (db.size() < 4)
                return p;
            const double binHz = rate / settings.fftSize;
            auto value = [&] (size_t k) { return isPower ? (db[k] > 1.0e-20f ? 10.0 * std::log10 (db[k]) : -200.0) : (double) db[k]; };
            const int cols = juce::roundToInt (plot.getWidth());
            const double lo = std::log (fMin()), hi = std::log (rate / 2.0);
            for (int c = 0; c <= cols; ++c)
            {
                const double f0 = std::exp (lo + (hi - lo) * c / cols), f1 = std::exp (lo + (hi - lo) * (c + 1) / cols);
                const double b0 = f0 / binHz, b1 = f1 / binHz;
                double v;
                if (b1 - b0 < 1.0)
                {
                    const auto k = (size_t) juce::jlimit (0.0, (double) db.size() - 2, std::floor (b0));
                    const double frac = juce::jlimit (0.0, 1.0, b0 - (double) k);
                    v = value (k) + (value (k + 1) - value (k)) * frac;
                }
                else
                {
                    v = -300.0;
                    for (auto k = (size_t) std::floor (b0); k <= (size_t) std::ceil (b1) && k < db.size(); ++k)
                        v = juce::jmax (v, value (k));
                }
                const float y = juce::jlimit (plot.getY(), plot.getBottom(), yOf (v));
                const float x = plot.getX() + (float) c;
                if (c == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };

        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (plot.toNearestInt());
            if (! holdDb.empty())
                instr::strokeTrace (g, curve (holdDb, false), st.hold.withAlpha (0.7f), 1.0f);
            auto p = curve (avgPower, true);
            if (! p.isEmpty())
            {
                auto fill = p;
                fill.lineTo (plot.getRight(), plot.getBottom());
                fill.lineTo (plot.getX(), plot.getBottom());
                fill.closeSubPath();
                const auto c = stale ? st.spectrum.withMultipliedSaturation (0.3f) : st.spectrum;
                g.setGradientFill (juce::ColourGradient (c.withAlpha (0.28f), 0.0f, plot.getY(), c.withAlpha (0.02f), 0.0f, plot.getBottom(), false));
                g.fillPath (fill);
                instr::strokeTrace (g, p, c, 1.2f);
            }
            // Peak markers
            for (size_t i = 0; i < peaks.size(); ++i)
            {
                const float x = xOf (peaks[i].frequency, plot), y = yOf (peaks[i].db);
                juce::Path m;
                m.addTriangle (x - 4.0f, y - 10.0f, x + 4.0f, y - 10.0f, x, y - 4.0f);
                g.setColour (i == 0 ? st.ch1 : st.label);
                g.fillPath (m);
            }
        }

        if (stale)
        {
            g.setColour (theme.warn);
            g.setFont (theme.monoFont (11.0f));
            g.drawText ("NO SIGNAL", plot.reduced (6.0f, 4.0f), juce::Justification::topRight);
        }

        // Readouts: the three strongest peaks
        auto strip = getLocalBounds().reduced (10, 8).toFloat().withTop (outer.getBottom() + 6.0f);
        g.setFont (theme.monoFont (12.0f));
        for (size_t i = 0; i < 3; ++i)
        {
            auto line = strip.removeFromTop (16.0f);
            if (i < peaks.size())
            {
                g.setColour (i == 0 ? st.ch1 : theme.chromeText);
                g.drawText (juce::String (i + 1) + "  " + instr::eng (peaks[i].frequency, "Hz", 5).paddedLeft (' ', 11)
                                + "   " + juce::String (peaks[i].db, 1).paddedLeft (' ', 6) + " dBFS",
                            line, juce::Justification::centredLeft);
            }
            else if (i == 0)
            {
                g.setColour (st.label);
                g.drawText ("no peaks", line, juce::Justification::centredLeft);
            }
        }
        g.setColour (st.label);
        g.setFont (theme.monoFont (10.5f));
        g.drawText ("Hann  " + juce::String (rate / settings.fftSize, 2) + " Hz/bin", strip.removeFromTop (14.0f), juce::Justification::centredLeft);
    }
}
