#pragma once

#include "config/Config.h"
#include "emu/CvBridge.h"
#include "emu/MidiBridge.h"
#include "emu/MidiStreamParser.h"
#include "emu/UartServer.h"

#include <deque>
#include <thread>
#include <mutex>

namespace kickemu
{
    /** Ring of interleaved multi-channel frames; one writer, any number of readers. */
    class FrameRing
    {
    public:
        FrameRing (int channels, int capacityFrames, double sampleRate);

        void push (const float* frames, int numFrames);   // interleaved
        void clear();

        /** Copies the newest n frames of one channel (or the mean of channels 0 and 1
            when channel == -1) ending at endFrame (exclusive; < 0 = the newest).
            Returns the number copied (fewer when the ring holds less) and the end
            frame actually used. */
        int read (int channel, int n, float* dst, juce::int64 endFrame, juce::int64& usedEnd) const;

        juce::int64 getWritten() const;
        double getSampleRate() const { return rate; }
        int getChannels() const { return channels; }
        int getCapacity() const { return capacity; }
        /** Milliseconds since the last push (very large if never). */
        double msSinceLastWrite() const;

    private:
        const int channels, capacity;
        const double rate;
        mutable std::mutex lock;
        std::vector<float> data;
        juce::int64 written = 0;
        double lastWriteMs = -1.0e12;
    };

    /**
        Data sources of the measuring instruments (scope, spectrum analyser,
        multimeter) and the MIDI monitor.

        - Audio: the SAI2 playback tap (host-interface.md, "Playback tap"), read from
          audiotap.sock: S32LE stereo at 48 kHz in guest time, kept as float -1..1 (FS).
        - CV: the eight AD5671R outputs reported on cv.sock, sampled and held at
          host.instruments.cv_rate_hz (default 1000) on the host clock. The converter
          only reports changes, so steps are timed to about 1 ms.
        - MIDI: every message in both directions while the monitor is enabled.
    */
    class InstrumentHub
    {
    public:
        InstrumentHub (const Config& config, CvBridge& cv, MidiBridge& midi);
        ~InstrumentHub();

        bool start (juce::String& error);
        void stop();

        /** A selectable signal: "audio_l", "audio_r", "audio_mid", "cv1".."cv8". */
        struct Source
        {
            juce::String id, label, unit;   // unit "FS" for audio, "V" for CV
            bool audio = true;
            int channel = 0;                // ring channel (-1 = mid)
            double fullScale = 1.0;         // 1 FS or the CV range in volts
        };
        std::vector<Source> getSources() const;
        bool findSource (const juce::String& id, Source& out) const;

        const FrameRing& ringFor (const Source& s) const { return s.audio ? audio : cvRing; }

        /** Newest samples of a source covering durationSec. */
        struct Capture
        {
            std::vector<float> samples;
            double sampleRate = 0.0;
            juce::int64 endFrame = 0;
            double ageMs = 0.0;      // since the source last received data
        };
        bool capture (const juce::String& sourceId, double durationSec, Capture& out) const;

        bool isTapConnected() const { return tap.isConnected(); }
        bool isCvConnected() const { return cv.isConnected(); }
        juce::uint64 getTapFrames() const { return tapFrames.load(); }

        // ---- MIDI monitor ----------------------------------------------------------
        struct MidiEntry
        {
            juce::uint64 index;
            double time;                    // seconds since the hub started
            bool toGuest;                   // true: host -> KICK MIDI IN; false: KICK MIDI OUT -> host
            std::vector<juce::uint8> bytes;
        };
        void setMidiMonitorEnabled (bool on) { midiEnabled = on; }
        bool isMidiMonitorEnabled() const { return midiEnabled.load(); }
        std::vector<MidiEntry> midiSince (juce::uint64 index, size_t max) const;
        juce::uint64 midiEnd() const;
        void clearMidi();
        /** Feeds the monitor directly (public for tests). Raw bytes, any number of messages. */
        void recordMidi (bool toGuest, const juce::uint8* data, size_t size);

    private:
        void cvLoop();

        const Config& config;
        CvBridge& cv;
        MidiBridge& midi;

        FrameRing audio, cvRing;
        UartServer tap { "audiotap" };
        juce::uint8 partial[8] {};
        int partialLen = 0;
        std::atomic<juce::uint64> tapFrames { 0 };
        std::vector<float> convertBuffer;

        std::unique_ptr<std::thread> cvThread;
        std::atomic<bool> cvRunning { false };
        double cvRate;
        int cvChannels;

        std::atomic<bool> midiEnabled { false };
        mutable std::mutex midiLock;
        std::deque<MidiEntry> midiLog;
        juce::uint64 midiTotal = 0;
        std::mutex parserLock;
        MidiStreamParser toGuestParser;
        const double startMs = juce::Time::getMillisecondCounterHiRes();
    };
}
