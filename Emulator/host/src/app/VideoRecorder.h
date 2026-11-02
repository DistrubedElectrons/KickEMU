#pragma once

#include "app/Peripherals.h"
#include "config/Config.h"

#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include <condition_variable>
#include <deque>
#include <thread>

namespace kickemu
{
    /**
        Records what the device shows, with the guest's audio output, to an MP4.

        Frames come from a FrameSource (the device view, or the bare panel in headless
        mode), taken on the message thread at a fixed rate and piped as raw BGRA into
        ffmpeg (H.264). Time is wall-clock: when a frame comes late, the previous one is
        repeated, so the video always runs as long as the recording did. Audio is the
        SAI playback path, written by QEMU to a WAV (machine property audio-wav-out);
        on stop the two are muxed (AAC, the audio padded to the video's length).

        ffmpeg is an external program: host.video.ffmpeg, else PATH, /opt/homebrew/bin,
        /usr/local/bin, /usr/bin. A running WAV recording (Peripherals) and video audio
        share the same QEMU property, so the video is recorded without sound then.
    */
    class VideoRecorder : private juce::Timer
    {
    public:
        using FrameSource = std::function<juce::Image()>;
        using Done = std::function<void (const juce::String& error, const juce::File& file)>;

        VideoRecorder (Config& config, Peripherals& peripherals, std::function<void (const juce::String&)> log);
        ~VideoRecorder() override;

        /** Starts recording to @file (.mp4). Message thread. withAudio needs QEMU running. */
        juce::Result start (const juce::File& file, FrameSource source, bool withAudio, bool qemuRunning);
        /** Stops; @done is called on the message thread once the MP4 is complete. */
        void stop (Done done);

        bool isRecording() const { return recording; }
        bool isFinishing() const { return finishing; }
        double getElapsedSeconds() const;
        juce::File getFile() const { return target; }
        bool hasAudio() const { return audio; }
        juce::int64 getFramesWritten() const { return framesQueued; }

        static juce::File findFfmpeg (const Config& config);

        /** Called on the message thread when recording starts or ends. */
        std::function<void()> onChanged;

    private:
        void timerCallback() override;
        void writerLoop();
        void finish (Done done);
        void changed() { if (onChanged) onChanged(); }

        struct Frame
        {
            juce::Image image;
            int repeat = 1;
        };

        Config& config;
        Peripherals& peripherals;
        std::function<void (const juce::String&)> log;

        FrameSource source;
        juce::File target, workDir, videoFile, audioFile, ffmpeg;
        int fps = 30, width = 0, height = 0;
        bool recording = false, finishing = false, audio = false;
        double startedAt = 0.0;
        juce::int64 framesQueued = 0;

        int pipeFd = -1, encoderPid = -1;
        std::thread writer, finisher;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
        std::mutex lock;
        std::condition_variable wake;
        std::deque<Frame> queue;
        bool endOfStream = false;
        std::atomic<bool> writeFailed { false };
    };
}
