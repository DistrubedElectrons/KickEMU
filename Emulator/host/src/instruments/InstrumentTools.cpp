#include "instruments/InstrumentTools.h"

#include "app/MainComponent.h"
#include "instruments/InstrumentsView.h"
#include "instruments/SignalAnalysis.h"

namespace kickemu
{
    namespace
    {
        juce::String sourceList (InstrumentHub& hub)
        {
            juce::StringArray ids;
            for (auto& s : hub.getSources())
                ids.add (s.id);
            return ids.joinIntoString (", ");
        }

        bool runOnMessageThread (std::function<void()> fn, int timeoutMs)
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            {
                fn();
                return true;
            }
            auto done = std::make_shared<juce::WaitableEvent>();
            juce::MessageManager::callAsync ([action = std::move (fn), done] { action(); done->signal(); });
            return done->wait (timeoutMs);
        }

        juce::var number (double v) { return std::isfinite (v) ? juce::var (v) : juce::var(); }
    }

    void addInstrumentTools (ToolRegistry& tools, Session& session)
    {
        auto& hub = session.instruments;
        const auto sources = "audio_l, audio_r, audio_mid (playback tap, FS = full scale) or cv1..cv8 (CV/GATE outputs 1..8 "
                             "as on the rear panel = cv_read channels 0..7, volts)";

        tools.add ({ "measure",
                     juce::String ("Multimeter reading of a source over the newest window: DC (mean), RMS, AC RMS, peak, min/max and "
                                   "frequency from zero crossings. Audio also in dBFS. Sources: ") + sources + ".",
                     Schema().string ("source", "source id", true).integer ("window_ms", "averaging window, default 250"),
                     [&hub] (const juce::var& a)
                     {
                         InstrumentHub::Source s;
                         if (! hub.findSource (args::str (a, "source"), s))
                             return ToolResult::fail ("unknown source; use one of " + sourceList (hub));
                         InstrumentHub::Capture cap;
                         hub.capture (s.id, juce::jlimit (1, 10000, args::integer (a, "window_ms", 250)) / 1000.0, cap);
                         const auto m = signal::measure (cap.samples.data(), (int) cap.samples.size(), cap.sampleRate);
                         auto r = object ({ { "source", s.id }, { "unit", s.unit }, { "sample_rate", cap.sampleRate },
                                            { "samples", m.samples }, { "dc", m.mean }, { "rms", m.rms }, { "ac_rms", m.acRms },
                                            { "peak", m.peak }, { "min", m.min }, { "max", m.max },
                                            { "frequency_hz", m.frequency > 0.0 ? juce::var (m.frequency) : juce::var() },
                                            { "age_ms", number (cap.ageMs) },
                                            { "connected", s.audio ? hub.isTapConnected() : hub.isCvConnected() } });
                         if (s.audio)
                         {
                             r.getDynamicObject()->setProperty ("rms_dbfs", signal::toDb (m.rms));
                             r.getDynamicObject()->setProperty ("peak_dbfs", signal::toDb (m.peak));
                         }
                         return ToolResult::ok (r);
                     } });

        tools.add ({ "spectrum",
                     "FFT of a source (Hann window, Welch average of 50 % overlapped blocks over the window, dBFS: a full-scale "
                     "sine reads 0 dB). Returns the strongest peaks with interpolated frequency and level.",
                     Schema().string ("source", "source id, default audio_mid")
                             .integer ("fft_size", "power of two 256..65536, default 8192")
                             .integer ("window_ms", "analysed span, default 1000")
                             .integer ("peaks", "number of peaks, default 5")
                             .number ("min_db", "ignore peaks below this level, default -120"),
                     [&hub] (const juce::var& a)
                     {
                         InstrumentHub::Source s;
                         if (! hub.findSource (args::str (a, "source", "audio_mid"), s))
                             return ToolResult::fail ("unknown source; use one of " + sourceList (hub));
                         const int fftSize = args::integer (a, "fft_size", 8192);
                         if (! juce::isPowerOfTwo (fftSize) || fftSize < 256 || fftSize > 65536)
                             return ToolResult::fail ("fft_size must be a power of two between 256 and 65536");
                         InstrumentHub::Capture cap;
                         hub.capture (s.id, juce::jlimit (1, 10000, args::integer (a, "window_ms", 1000)) / 1000.0, cap);
                         if (cap.samples.size() < 16)
                             return ToolResult::fail ("no samples from " + s.id + " yet");
                         const auto spec = signal::spectrum (cap.samples.data(), (int) cap.samples.size(), cap.sampleRate, fftSize);
                         juce::Array<juce::var> list;
                         for (auto& p : signal::findPeaks (spec, juce::jlimit (1, 64, args::integer (a, "peaks", 5)), args::number (a, "min_db", -120.0)))
                             list.add (object ({ { "frequency_hz", p.frequency }, { "dbfs", p.db } }));
                         auto sorted = spec.db;
                         std::nth_element (sorted.begin(), sorted.begin() + (long) sorted.size() / 2, sorted.end());
                         return ToolResult::ok (object ({ { "source", s.id }, { "sample_rate", cap.sampleRate }, { "fft_size", fftSize },
                                                          { "bin_hz", spec.binHz() }, { "samples", (int) cap.samples.size() },
                                                          { "median_db", (double) sorted[sorted.size() / 2] }, { "peaks", list },
                                                          { "age_ms", number (cap.ageMs) } }));
                     } });

        tools.add ({ "midi_monitor",
                     "MIDI monitor in both directions (dir 'in' = into the KICK's MIDI IN, 'out' = sent by the KICK), decoded. "
                     "Recording only happens while enabled; the GUI's MIDI page shares this state.",
                     Schema().boolean ("enable", "switch recording on or off")
                             .boolean ("clear", "drop the recorded messages first")
                             .integer ("since", "first index to return (from a previous 'next')")
                             .integer ("max", "default 256"),
                     [&hub] (const juce::var& a)
                     {
                         if (args::has (a, "enable"))
                             hub.setMidiMonitorEnabled (args::boolean (a, "enable"));
                         if (args::boolean (a, "clear"))
                             hub.clearMidi();
                         auto entries = hub.midiSince ((juce::uint64) juce::jmax (0, args::integer (a, "since")), (size_t) juce::jlimit (1, 4096, args::integer (a, "max", 256)));
                         juce::Array<juce::var> list;
                         for (auto& e : entries)
                         {
                             const auto t = signal::describeMidi (e.bytes.data(), e.bytes.size());
                             juce::StringArray hex;
                             for (auto b : e.bytes)
                                 hex.add (juce::String::toHexString (b).paddedLeft ('0', 2));
                             list.add (object ({ { "index", (juce::int64) e.index }, { "time", e.time }, { "dir", e.toGuest ? "in" : "out" },
                                                 { "kind", t.kind }, { "channel", t.channel }, { "text", t.text },
                                                 { "bytes", hex.joinIntoString (" ") } }));
                         }
                         return ToolResult::ok (object ({ { "enabled", hub.isMidiMonitorEnabled() }, { "messages", list },
                                                          { "next", (juce::int64) (entries.empty() ? hub.midiEnd() : entries.back().index + 1) } }));
                     } });

        tools.add ({ "instrument_view",
                     "Render an instrument page (scope, spectrum, meter, midi) to PNG offscreen, as the side panel draws it. "
                     "Scope: ch1/ch2 sources, ch1_div/ch2_div, timebase_ms per division, trigger_level, trigger (1 or 2). "
                     "Spectrum: source, fft_size. Meter: source (A) and source_b (B), window_ms.",
                     Schema().enumeration ("view", { "scope", "spectrum", "meter", "midi" }, "page", true)
                             .string ("path", "also write the PNG here")
                             .integer ("width", "default 460").integer ("height", "default 640")
                             .string ("ch1", "scope CH1 source ('' = off)").string ("ch2", "scope CH2 source ('' = off)")
                             .number ("ch1_div", "CH1 units per division").number ("ch2_div", "CH2 units per division")
                             .number ("timebase_ms", "scope time per division, default 1")
                             .number ("trigger_level", "in the trigger channel's unit").integer ("trigger", "trigger channel 1 or 2")
                             .string ("source", "spectrum / meter A source").string ("source_b", "meter B source")
                             .integer ("fft_size", "spectrum FFT size").integer ("window_ms", "meter window"),
                     [&session] (const juce::var& a)
                     {
                         ToolResult result;
                         const bool ran = runOnMessageThread ([&]
                         {
                             // Leaked on purpose: no static destruction after JUCE has shut down.
                             static auto* theme = new Theme();
                             static auto* lnf = new ChromeLookAndFeel (*theme);
                             InstrumentsView view (session.instruments, *theme, false);
                             view.setLookAndFeel (lnf);
                             view.setSize (juce::jlimit (300, 2000, args::integer (a, "width", 460)), juce::jlimit (300, 2000, args::integer (a, "height", 640)));
                             const auto page = args::str (a, "view");
                             if (page == "scope")
                             {
                                 auto st = view.getScope().getSettings();
                                 if (args::has (a, "ch1")) st.ch[0].source = args::str (a, "ch1");
                                 if (args::has (a, "ch2")) st.ch[1].source = args::str (a, "ch2");
                                 InstrumentHub::Source src;
                                 for (int c = 0; c < 2; ++c)
                                     if (session.instruments.findSource (st.ch[c].source, src))
                                         st.ch[c].perDiv = src.audio ? 0.25 : 1.0;
                                 if (args::has (a, "ch1_div")) st.ch[0].perDiv = args::number (a, "ch1_div");
                                 if (args::has (a, "ch2_div")) st.ch[1].perDiv = args::number (a, "ch2_div");
                                 st.secPerDiv = args::number (a, "timebase_ms", 1.0) / 1000.0;
                                 st.triggerChannel = juce::jlimit (0, 1, args::integer (a, "trigger", 1) - 1);
                                 st.triggerLevel = args::number (a, "trigger_level", 0.0);
                                 view.getScope().setSettings (st);
                                 view.showPage (InstrumentsView::scope);
                                 view.getScope().acquire();
                             }
                             else if (page == "spectrum")
                             {
                                 auto st = view.getSpectrum().getSettings();
                                 st.source = args::str (a, "source", st.source);
                                 st.fftSize = args::integer (a, "fft_size", st.fftSize);
                                 view.getSpectrum().setSettings (st);
                                 view.showPage (InstrumentsView::spectrum);
                                 view.getSpectrum().acquire();
                             }
                             else if (page == "meter")
                             {
                                 const double w = args::integer (a, "window_ms", 250) / 1000.0;
                                 view.getMeter().setInput (0, { args::str (a, "source", "audio_l"), w });
                                 view.getMeter().setInput (1, { args::str (a, "source_b", "cv1"), w });
                                 view.showPage (InstrumentsView::meter);
                             }
                             else
                             {
                                 view.showPage (InstrumentsView::midi);
                                 view.getMidi().poll();
                             }
                             auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);
                             view.setLookAndFeel (nullptr);
                             juce::MemoryOutputStream png;
                             juce::PNGImageFormat().writeImageToStream (image, png);
                             result = ToolResult::ok (object ({ { "view", page }, { "width", image.getWidth() }, { "height", image.getHeight() } }));
                             result.png = png.getMemoryBlock();
                         }, 10000);
                         if (! ran)
                             return ToolResult::fail ("the message thread did not respond");
                         if (auto path = args::str (a, "path"); path.isNotEmpty() && ! result.failed())
                         {
                             auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
                             if (! file.replaceWithData (result.png.getData(), result.png.getSize()))
                                 return ToolResult::fail ("cannot write " + file.getFullPathName());
                             result.data.getDynamicObject()->setProperty ("path", file.getFullPathName());
                         }
                         return result;
                     } });
    }
}
