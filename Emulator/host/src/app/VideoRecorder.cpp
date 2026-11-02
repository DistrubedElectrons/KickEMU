#include "app/VideoRecorder.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace kickemu
{
    namespace
    {
        /** Starts @program with @args; its stdin is a new pipe when @stdinFd is given. */
        int spawn (const juce::File& program, const juce::StringArray& args, const juce::File& logFile, int* stdinFd)
        {
            int fds[2] = { -1, -1 };
            if (stdinFd != nullptr && ::pipe (fds) != 0)
                return -1;

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init (&actions);
            if (stdinFd != nullptr)
            {
                posix_spawn_file_actions_adddup2 (&actions, fds[0], 0);
                posix_spawn_file_actions_addclose (&actions, fds[0]);
                posix_spawn_file_actions_addclose (&actions, fds[1]);
            }
            else
            {
                posix_spawn_file_actions_addopen (&actions, 0, "/dev/null", O_RDONLY, 0);
            }
            const auto log = logFile.getFullPathName();
            posix_spawn_file_actions_addopen (&actions, 1, log.toRawUTF8(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            posix_spawn_file_actions_adddup2 (&actions, 1, 2);

            std::vector<std::string> storage { program.getFullPathName().toStdString() };
            for (auto& a : args)
                storage.push_back (a.toStdString());
            std::vector<char*> argv;
            for (auto& s : storage)
                argv.push_back (s.data());
            argv.push_back (nullptr);

            pid_t pid = -1;
            const int rc = posix_spawn (&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy (&actions);
            if (stdinFd != nullptr)
            {
                ::close (fds[0]);
                if (rc != 0)
                    ::close (fds[1]);
                else
                    *stdinFd = fds[1];
            }
            return rc == 0 ? (int) pid : -1;
        }

        int waitFor (int pid)
        {
            int status = 0;
            while (::waitpid (pid, &status, 0) < 0)
                if (errno != EINTR)
                    return -1;
            return WIFEXITED (status) ? WEXITSTATUS (status) : 128 + WTERMSIG (status);
        }

        juce::String tail (const juce::File& f)
        {
            auto lines = juce::StringArray::fromLines (f.loadFileAsString().trim());
            lines.removeRange (0, lines.size() - 3);
            return lines.joinIntoString (" | ");
        }
    }

    VideoRecorder::VideoRecorder (Config& c, Peripherals& p, std::function<void (const juce::String&)> l)
        : config (c), peripherals (p), log (std::move (l))
    {
    }

    VideoRecorder::~VideoRecorder()
    {
        *alive = false;
        stopTimer();
        const bool wasRecording = recording;
        {
            std::lock_guard<std::mutex> g (lock);
            endOfStream = true;
        }
        wake.notify_all();
        if (finisher.joinable())
            finisher.join();
        if (writer.joinable())
            writer.join();
        if (encoderPid > 0 && waitFor (encoderPid) == 0 && wasRecording)
        {
            // Quit while recording: keep the video (QEMU is gone, so no audio mux).
            target.deleteFile();
            if (videoFile.moveFileTo (target))
                workDir.deleteRecursively();
        }
    }

    juce::File VideoRecorder::findFfmpeg (const Config& config)
    {
        if (auto configured = config.getString ("host.video.ffmpeg", ""); configured.isNotEmpty())
            return juce::File (configured);
        juce::StringArray dirs;
        dirs.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", ""), ":", "");
        // A GUI app started from Finder does not inherit the shell's PATH.
        dirs.addArray ({ "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" });
        for (auto& d : dirs)
            if (auto f = juce::File (d).getChildFile ("ffmpeg"); d.isNotEmpty() && f.existsAsFile())
                return f;
        return {};
    }

    double VideoRecorder::getElapsedSeconds() const
    {
        return recording ? (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0 : 0.0;
    }

    juce::Result VideoRecorder::start (const juce::File& file, FrameSource src, bool withAudio, bool qemuRunning)
    {
        if (recording || finishing)
            return juce::Result::fail ("A recording is already running");
        ffmpeg = findFfmpeg (config);
        if (! ffmpeg.existsAsFile())
            return juce::Result::fail ("ffmpeg not found (install it, or set host.video.ffmpeg)");

        auto first = src();
        if (! first.isValid())
            return juce::Result::fail ("Nothing to record yet (no frame)");

        // H.264 4:2:0 needs even dimensions; cap the width to keep the encoder real-time.
        const int maxWidth = config.getInt ("host.video.max_width", 1280);
        width = juce::jmin (maxWidth, first.getWidth()) & ~1;
        height = juce::roundToInt ((double) width * first.getHeight() / first.getWidth()) & ~1;
        fps = juce::jlimit (5, 60, config.getInt ("host.video.fps", 30));

        if (! file.getParentDirectory().createDirectory())
            return juce::Result::fail ("Cannot create " + file.getParentDirectory().getFullPathName());
        workDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getNonexistentChildFile ("kickemu-rec", "", false);
        if (! workDir.createDirectory())
            return juce::Result::fail ("Cannot create " + workDir.getFullPathName());
        videoFile = workDir.getChildFile ("video.mp4");
        audioFile = workDir.getChildFile ("audio.wav");
        target = file;

        // A dead encoder must not take the app down with SIGPIPE.
        std::signal (SIGPIPE, SIG_IGN);
        encoderPid = spawn (ffmpeg,
                            { "-hide_banner", "-loglevel", "error", "-y",
                              "-f", "rawvideo", "-pix_fmt", "bgra",
                              "-s", juce::String (width) + "x" + juce::String (height),
                              "-r", juce::String (fps), "-i", "-",
                              "-c:v", "libx264", "-preset", config.getString ("host.video.preset", "veryfast"),
                              "-crf", juce::String (config.getInt ("host.video.crf", 20)),
                              "-pix_fmt", "yuv420p", videoFile.getFullPathName() },
                            workDir.getChildFile ("ffmpeg.log"), &pipeFd);
        if (encoderPid <= 0)
            return juce::Result::fail ("Cannot start " + ffmpeg.getFullPathName());

        source = std::move (src);
        audio = false;
        if (withAudio && qemuRunning && peripherals.getWavOut() == juce::File())
        {
            audio = true;
            peripherals.setWavOut (audioFile, [this] (const juce::String& error)
            {
                if (error.isNotEmpty())
                {
                    audio = false;
                    log ("Video: no audio (" + error + ")");
                }
            });
        }
        else if (withAudio)
        {
            log (qemuRunning ? "Video: no audio, a WAV recording is already running"
                             : "Video: no audio, QEMU is not running");
        }

        endOfStream = false;
        writeFailed = false;
        queue.clear();
        framesQueued = 0;
        writer = std::thread ([this] { writerLoop(); });
        recording = true;
        startedAt = juce::Time::getMillisecondCounterHiRes();
        {
            std::lock_guard<std::mutex> g (lock);
            queue.push_back ({ std::move (first), 1 });
        }
        framesQueued = 1;
        wake.notify_all();
        startTimerHz (fps);
        log ("Video: recording " + juce::String (width) + "x" + juce::String (height) + " @ " + juce::String (fps)
             + " fps" + (audio ? " with audio" : "") + " to " + target.getFullPathName());
        changed();
        return juce::Result::ok();
    }

    void VideoRecorder::timerCallback()
    {
        if (writeFailed)
        {
            log ("Video: the encoder stopped (" + tail (workDir.getChildFile ("ffmpeg.log")) + ")");
            stop ([] (const juce::String&, const juce::File&) {});
            return;
        }
        const auto due = (juce::int64) (getElapsedSeconds() * fps) + 1;
        const int missing = (int) (due - framesQueued);
        if (missing <= 0)
            return;
        {
            std::lock_guard<std::mutex> g (lock);
            if (queue.size() >= 4)      // encoder behind: catch up with repeats on the next tick
                return;
        }
        auto image = source();
        std::lock_guard<std::mutex> g (lock);
        if (image.isValid())
            queue.push_back ({ std::move (image), missing });
        else if (! queue.empty())
            queue.back().repeat += missing;
        else
            return;
        framesQueued = due;
        wake.notify_all();
    }

    void VideoRecorder::writerLoop()
    {
        std::vector<juce::uint8> row ((size_t) width * 4);
        juce::Image last;
        for (;;)
        {
            Frame f;
            {
                std::unique_lock<std::mutex> g (lock);
                wake.wait (g, [this] { return endOfStream || ! queue.empty(); });
                if (queue.empty())
                    break;
                f = std::move (queue.front());
                queue.pop_front();
            }
            auto image = f.image;
            if (image.getWidth() != width || image.getHeight() != height)
                image = image.rescaled (width, height, juce::Graphics::mediumResamplingQuality);
            image = image.convertedToFormat (juce::Image::ARGB);
            const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
            for (int n = 0; n < f.repeat && ! writeFailed; ++n)
            {
                for (int y = 0; y < height && ! writeFailed; ++y)
                {
                    // JUCE ARGB is premultiplied B,G,R,A in memory on little-endian hosts.
                    const auto* line = data.getLinePointer (y);
                    for (int x = 0; x < width; ++x)
                    {
                        const auto* px = line + x * data.pixelStride;
                        auto* out = row.data() + x * 4;
                        out[0] = px[0]; out[1] = px[1]; out[2] = px[2]; out[3] = 255;
                    }
                    size_t done = 0;
                    while (done < row.size())
                    {
                        const auto w = ::write (pipeFd, row.data() + done, row.size() - done);
                        if (w < 0 && errno == EINTR)
                            continue;
                        if (w <= 0) { writeFailed = true; break; }
                        done += (size_t) w;
                    }
                }
            }
        }
        ::close (pipeFd);
        pipeFd = -1;
    }

    void VideoRecorder::stop (Done done)
    {
        if (! recording)
        {
            done ("Not recording", {});
            return;
        }
        stopTimer();
        recording = false;
        finishing = true;
        changed();

        auto finishAll = [this, done] (const juce::String&) { finish (done); };
        if (audio)
            peripherals.setWavOut ({}, finishAll);     // finalises the WAV header first
        else
            finishAll ({});
    }

    void VideoRecorder::finish (Done done)
    {
        {
            std::lock_guard<std::mutex> g (lock);
            endOfStream = true;
        }
        wake.notify_all();
        const auto seconds = (double) framesQueued / fps;
        if (finisher.joinable())
            finisher.join();
        finisher = std::thread ([this, done, seconds]
        {
            if (writer.joinable())
                writer.join();
            const int encoded = waitFor (encoderPid);
            encoderPid = -1;
            juce::String error;
            const auto logFile = workDir.getChildFile ("ffmpeg.log");
            if (encoded != 0 || ! videoFile.existsAsFile())
            {
                error = "ffmpeg failed (" + tail (logFile) + ")";
            }
            else if (audio && audioFile.getSize() > 44)
            {
                const int pid = spawn (ffmpeg,
                                       { "-hide_banner", "-loglevel", "error", "-y",
                                         "-i", videoFile.getFullPathName(), "-i", audioFile.getFullPathName(),
                                         "-map", "0:v", "-map", "1:a", "-c:v", "copy",
                                         "-c:a", "aac", "-b:a", "192k", "-af", "apad",
                                         "-t", juce::String (seconds, 3), "-movflags", "+faststart",
                                         target.getFullPathName() },
                                       logFile, nullptr);
                if (pid <= 0 || waitFor (pid) != 0)
                    error = "muxing audio failed (" + tail (logFile) + ")";
            }
            else
            {
                target.deleteFile();
                if (! videoFile.moveFileTo (target))
                    error = "cannot write " + target.getFullPathName();
            }
            if (error.isEmpty())
                workDir.deleteRecursively();
            juce::MessageManager::callAsync ([this, done, error, seconds, stillAlive = alive]
            {
                if (! *stillAlive)
                    return;
                finishing = false;
                log (error.isEmpty() ? "Video saved: " + target.getFullPathName() + " (" + juce::String (seconds, 1) + " s"
                                           + (audio ? ", with audio)" : ", no audio)")
                                     : "Video: " + error + "; partial files in " + workDir.getFullPathName());
                changed();
                done (error, error.isEmpty() ? target : juce::File());
            });
        });
    }
}
