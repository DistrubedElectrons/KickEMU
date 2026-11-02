#include "instruments/InstrumentHub.h"

#include <cmath>
#include <cstring>

namespace kickemu
{
    // ------------------------------------------------------------------------------------
    // FrameRing

    FrameRing::FrameRing (int ch, int cap, double r)
        : channels (ch), capacity (cap), rate (r), data ((size_t) (ch * cap), 0.0f) {}

    void FrameRing::push (const float* frames, int n)
    {
        std::lock_guard<std::mutex> guard (lock);
        // More than the capacity: only the newest frames matter.
        if (n > capacity)
        {
            frames += (size_t) (n - capacity) * (size_t) channels;
            written += n - capacity;
            n = capacity;
        }
        for (int i = 0; i < n; ++i)
        {
            const auto slot = (size_t) ((written + i) % capacity) * (size_t) channels;
            std::memcpy (&data[slot], frames + (size_t) i * (size_t) channels, sizeof (float) * (size_t) channels);
        }
        written += n;
        lastWriteMs = juce::Time::getMillisecondCounterHiRes();
    }

    void FrameRing::clear()
    {
        std::lock_guard<std::mutex> guard (lock);
        written = 0;
        lastWriteMs = -1.0e12;
    }

    juce::int64 FrameRing::getWritten() const
    {
        std::lock_guard<std::mutex> guard (lock);
        return written;
    }

    double FrameRing::msSinceLastWrite() const
    {
        std::lock_guard<std::mutex> guard (lock);
        return juce::Time::getMillisecondCounterHiRes() - lastWriteMs;
    }

    int FrameRing::read (int channel, int n, float* dst, juce::int64 endFrame, juce::int64& usedEnd) const
    {
        std::lock_guard<std::mutex> guard (lock);
        const auto oldest = juce::jmax ((juce::int64) 0, written - capacity);
        auto end = endFrame < 0 ? written : juce::jlimit (oldest, written, endFrame);
        const int count = (int) juce::jmin ((juce::int64) n, end - oldest);
        const auto first = end - count;
        for (int i = 0; i < count; ++i)
        {
            const float* f = &data[(size_t) ((first + i) % capacity) * (size_t) channels];
            dst[i] = channel < 0 ? 0.5f * (f[0] + f[1]) : f[juce::jlimit (0, channels - 1, channel)];
        }
        usedEnd = end;
        return count;
    }

    // ------------------------------------------------------------------------------------
    // InstrumentHub

    InstrumentHub::InstrumentHub (const Config& c, CvBridge& cvBridge, MidiBridge& midiBridge)
        : config (c), cv (cvBridge), midi (midiBridge),
          audio (2, juce::jmax (4096, (int) (c.getDouble ("host.instruments.audio_rate_hz", 48000.0) * c.getDouble ("host.instruments.audio_seconds", 4.0))),
                 c.getDouble ("host.instruments.audio_rate_hz", 48000.0)),
          cvRing (juce::jlimit (1, CvBridge::maxChannels, cvBridge.getNumChannels()),
                  juce::jmax (1024, (int) (c.getDouble ("host.instruments.cv_rate_hz", 1000.0) * c.getDouble ("host.instruments.cv_seconds", 10.0))),
                  juce::jlimit (10.0, 10000.0, c.getDouble ("host.instruments.cv_rate_hz", 1000.0)))
    {
        cvRate = cvRing.getSampleRate();
        cvChannels = cvRing.getChannels();

        tap.onData = [this] (const juce::uint8* d, size_t n)
        {
            // Raw S32LE stereo frames; a read may end inside a frame.
            convertBuffer.clear();
            size_t i = 0;
            while (i < n)
            {
                const size_t take = juce::jmin ((size_t) (8 - partialLen), n - i);
                std::memcpy (partial + partialLen, d + i, take);
                partialLen += (int) take;
                i += take;
                if (partialLen == 8)
                {
                    for (int s = 0; s < 2; ++s)
                    {
                        const auto* p = partial + 4 * s;
                        const auto v = (juce::int32) ((juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24));
                        convertBuffer.push_back ((float) ((double) v / 2147483648.0));
                    }
                    partialLen = 0;
                }
            }
            const int frames = (int) convertBuffer.size() / 2;
            if (frames > 0)
            {
                audio.push (convertBuffer.data(), frames);
                tapFrames += (juce::uint64) frames;
            }
        };
        tap.onConnectionChanged = [this] (bool) { partialLen = 0; };

        toGuestParser.onMessage = [this] (const juce::uint8* d, size_t n)
        {
            std::lock_guard<std::mutex> guard (midiLock);
            midiLog.push_back ({ midiTotal++, (juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001, true, { d, d + n } });
            while (midiLog.size() > 4096)
                midiLog.pop_front();
        };
        midi.onMonitor = [this] (bool toGuest, const juce::uint8* d, size_t n)
        {
            if (midiEnabled.load())
                recordMidi (toGuest, d, n);
        };
    }

    InstrumentHub::~InstrumentHub()
    {
        midi.onMonitor = nullptr;
        stop();
    }

    bool InstrumentHub::start (juce::String& error)
    {
        if (! tap.start (config.getSocketPath ("audiotap"), error))
            return false;
        if (! cvRunning.exchange (true))
            cvThread = std::make_unique<std::thread> ([this] { cvLoop(); });
        return true;
    }

    void InstrumentHub::stop()
    {
        cvRunning = false;
        if (cvThread != nullptr && cvThread->joinable())
            cvThread->join();
        cvThread.reset();
        tap.stop();
    }

    void InstrumentHub::cvLoop()
    {
        // Sample and hold of the converter outputs on the host clock. Catch-up after a
        // stall is bounded so a sleeping machine does not flood the ring.
        const double periodMs = 1000.0 / cvRate;
        double next = juce::Time::getMillisecondCounterHiRes();
        std::vector<float> frame ((size_t) cvChannels);
        while (cvRunning.load())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - next > 100.0)
                next = now;
            while (next <= now)
            {
                for (int ch = 0; ch < cvChannels; ++ch)
                    frame[(size_t) ch] = cv.getOutputVolts (ch);
                cvRing.push (frame.data(), 1);
                next += periodMs;
            }
            std::this_thread::sleep_for (std::chrono::microseconds ((int) juce::jmax (200.0, (next - now) * 1000.0)));
        }
    }

    std::vector<InstrumentHub::Source> InstrumentHub::getSources() const
    {
        std::vector<Source> out;
        out.push_back ({ "audio_l", "Audio L", "FS", true, 0, 1.0 });
        out.push_back ({ "audio_r", "Audio R", "FS", true, 1, 1.0 });
        out.push_back ({ "audio_mid", "Audio (L+R)/2", "FS", true, -1, 1.0 });
        for (int ch = 0; ch < cvChannels; ++ch)
            out.push_back ({ "cv" + juce::String (ch + 1), "CV " + juce::String (ch + 1), "V", false, ch, (double) cv.getFullScale() });
        return out;
    }

    bool InstrumentHub::findSource (const juce::String& id, Source& out) const
    {
        for (auto& s : getSources())
            if (s.id.equalsIgnoreCase (id))
            {
                out = s;
                return true;
            }
        return false;
    }

    bool InstrumentHub::capture (const juce::String& sourceId, double durationSec, Capture& out) const
    {
        Source s;
        if (! findSource (sourceId, s))
            return false;
        auto& ring = ringFor (s);
        const int n = juce::jlimit (1, ring.getCapacity(), (int) std::ceil (durationSec * ring.getSampleRate()));
        out.samples.resize ((size_t) n);
        const int got = ring.read (s.channel, n, out.samples.data(), -1, out.endFrame);
        out.samples.resize ((size_t) got);
        out.sampleRate = ring.getSampleRate();
        out.ageMs = ring.msSinceLastWrite();
        return true;
    }

    // ------------------------------------------------------------------------------------
    // MIDI monitor

    void InstrumentHub::recordMidi (bool toGuest, const juce::uint8* d, size_t n)
    {
        if (toGuest)
        {
            // Raw bytes written to the UART: split like the guest's UART receiver would.
            std::lock_guard<std::mutex> guard (parserLock);
            toGuestParser.feed (d, n);
            return;
        }
        std::lock_guard<std::mutex> guard (midiLock);
        midiLog.push_back ({ midiTotal++, (juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001, false, { d, d + n } });
        while (midiLog.size() > 4096)
            midiLog.pop_front();
    }

    std::vector<InstrumentHub::MidiEntry> InstrumentHub::midiSince (juce::uint64 index, size_t max) const
    {
        std::lock_guard<std::mutex> guard (midiLock);
        std::vector<MidiEntry> out;
        for (auto& e : midiLog)
            if (e.index >= index && out.size() < max)
                out.push_back (e);
        return out;
    }

    juce::uint64 InstrumentHub::midiEnd() const
    {
        std::lock_guard<std::mutex> guard (midiLock);
        return midiTotal;
    }

    void InstrumentHub::clearMidi()
    {
        std::lock_guard<std::mutex> guard (midiLock);
        midiLog.clear();   // indices keep counting so readers' cursors stay valid
    }
}
