#include "app/Session.h"

#include "emu/UnixSocket.h"
#include <cstdio>

namespace kickemu
{
    // ------------------------------------------------------------------------------------
    // LogBuffer

    void LogBuffer::add (const juce::String& source, const juce::String& text)
    {
        Entry e { juce::Time::getCurrentTime(), source, text };
        if (echoToStdout)
        {
            auto line = "[" + source + "] " + text + "\n";
            std::fputs (line.toRawUTF8(), stdout);
            std::fflush (stdout);
        }
        std::lock_guard<std::mutex> guard (lock);
        entries.push_back (std::move (e));
        while ((int) entries.size() > limit)
            entries.pop_front();
        total++;
    }

    std::vector<LogBuffer::Entry> LogBuffer::since (juce::uint64& cursor) const
    {
        std::lock_guard<std::mutex> guard (lock);
        std::vector<Entry> out;
        const auto t = total.load();
        const auto available = (juce::uint64) entries.size();
        const auto first = t - available;
        if (cursor < first)
            cursor = first;
        for (auto i = cursor; i < t; ++i)
            out.push_back (entries[(size_t) (i - first)]);
        cursor = t;
        return out;
    }

    void LogBuffer::clear()
    {
        std::lock_guard<std::mutex> guard (lock);
        entries.clear();
        total = 0;
    }

    // ------------------------------------------------------------------------------------
    // Session

    Session::Session (Config& c)
        : config (c),
          log (c.getInt ("host.ui.log_lines", 5000)),
          mcu (panel, c),
          midi (c),
          cv (c),
          console (c, "console"),
          uart2 (c, "uart2"),
          rfb (framebuffer)
    {
        mcu.onLog = [this] (const juce::String& s) { log.add ("mcu", s); };
        midi.onLog = [this] (const juce::String& s) { log.add ("midi", s); };
        cv.onLog = [this] (const juce::String& s) { log.add ("cv", s); };
        qmp.onLog = [this] (const juce::String& s) { log.add ("qmp", s); };
        rfb.onLog = [this] (const juce::String& s) { log.add ("vnc", s); };
        qmp.onEvent = [this] (const juce::String& event, const juce::var& data)
        {
            log.add ("qmp", "event " + event + (data.isVoid() ? juce::String() : " " + juce::JSON::toString (data, true)));
        };

        qemu.onOutput = [this] (const juce::String& line) { log.add ("qemu", line); };
        qemu.onExit = [this] (int code)
        {
            if (! shuttingDown.load())
                juce::MessageManager::callAsync ([this, code] { processExited (code); });
        };

        setupIoStatus();
    }

    Session::~Session()
    {
        shuttingDown = true;
        stopTimer();
        if (qemu.isRunning())
        {
            qmp.execute ("quit");
            if (! qemu.waitForExit (config.getInt ("host.qemu_quit_timeout_ms", 3000)))
                qemu.kill();
            qemu.waitForExit (1000);
        }
        rfb.stop();
        qmp.stop();
        uart2.stop();
        console.stop();
        cv.stop();
        midi.stop();
        mcu.stop();
    }

    void Session::setupIoStatus()
    {
        io.powered = [this] { return qemuState.load() == QemuState::running; };
        io.midiInCount = [this] { return midi.messagesToGuest.load(); };
        io.midiOutCount = [this] { return midi.messagesFromGuest.load(); };
        io.midiInRoute = [this]
        {
            juce::String s = "from \"" + midi.getVirtualInName() + "\"";
            if (midi.getInputDevice().isNotEmpty())
                s << " and the selected input";
            return s;
        };
        io.midiOutRoute = [this]
        {
            juce::String s = "to \"" + midi.getVirtualOutName() + "\"";
            if (midi.getOutputDevice().isNotEmpty())
                s << " and the selected output";
            return s;
        };
        io.networkUp = [this] { return qemuState.load() == QemuState::running && config.getBool ("qemu.network", false); };
        io.networkInfo = [this]
        {
            if (! config.getBool ("qemu.network", false))
                return juce::String ("disabled (qemu.network)");
            return "user network, ssh on localhost:" + juce::String (config.getInt ("qemu.ssh_host_port", 0));
        };
        io.cvOut = [this] (int ch) { return cv.getOutputVolts (ch); };
        io.cvIn = [this] (int ch) { return cv.getInputVolts (ch); };
        io.cvSerial = [this] (int ch) { return cv.getOutputSerial (ch); };
        io.cvChannels = [this] { return cv.getNumChannels(); };
        io.cvConnected = [this] { return cv.isConnected(); };
        io.audioRoute = [this]
        {
            auto dev = config.getString ("qemu.audiodev");
            return dev.isEmpty() ? juce::String ("no audio backend configured (qemu.audiodev)")
                                 : "QEMU " + dev + " backend (system default device)";
        };
        io.sdImage = [this] { return sdImage; };
    }

    juce::Result Session::startBridges()
    {
        auto runDir = config.getRunDir();
        if (! runDir.createDirectory())
            return juce::Result::fail ("Cannot create run directory " + runDir.getFullPathName());

        for (auto name : { "mcu", "uart2", "midi", "console", "qmp", "vnc", "cv" })
            if (! unixsocket::pathFits (config.getSocketPath (name).getFullPathName()))
                return juce::Result::fail ("Socket path too long for macOS (104 bytes): " + config.getSocketPath (name).getFullPathName()
                                           + ". Set run_dir to a shorter directory.");

        juce::String error;
        if (! mcu.start (error)) return juce::Result::fail ("MCU socket: " + error);
        if (! midi.start (error)) return juce::Result::fail ("MIDI socket: " + error);
        if (! cv.start (error)) return juce::Result::fail ("CV socket: " + error);
        if (! console.start (error)) return juce::Result::fail ("Console socket: " + error);
        if (! uart2.start (error)) return juce::Result::fail ("UART2 socket: " + error);

        log.add ("host", "Listening in " + runDir.getFullPathName());
        return juce::Result::ok();
    }

    bool Session::startQemu()
    {
        if (qemuState.load() != QemuState::stopped)
            return false;

        auto cmd = QemuCommandLine::build (config);
        for (auto& w : cmd.warnings)
            log.add ("host", "Warning: " + w);
        lastCommand = cmd.toShellString();
        if (! cmd.ok())
        {
            log.add ("host", "Cannot start QEMU: " + cmd.error);
            return false;
        }

        // SD shown in the front view only when the image was attached.
        sdImage = cmd.args.joinIntoString (" ").contains ("id=sd,") ? config.getPath ("storage.sd_image").getFileName() : juce::String();

        // QEMU creates the QMP/VNC sockets; remove stale ones from a crashed run.
        unixsocket::removeSocketFile (config.getSocketPath ("qmp").getFullPathName());
        unixsocket::removeSocketFile (config.getSocketPath ("vnc").getFullPathName());

        log.add ("host", "Starting: " + lastCommand);
        juce::String error;
        if (! qemu.start (cmd.args, error))
        {
            log.add ("host", error);
            return false;
        }

        qemuState = QemuState::running;
        const int retry = config.getInt ("host.connect_retry_ms", 300);
        qmp.start (config.getSocketPath ("qmp"), retry);
        if (config.getString ("qemu.display", "vnc") == "vnc")
            rfb.start (config.getSocketPath ("vnc"), retry);
        if (onStateChanged)
            onStateChanged();
        return true;
    }

    void Session::stopQemu()
    {
        if (qemuState.load() != QemuState::running)
            return;
        qemuState = QemuState::stopping;
        stopRequestedAt = juce::Time::getMillisecondCounter();
        log.add ("host", "Stopping QEMU");
        if (qmp.isReady())
            qmp.execute ("quit");
        else
            qemu.kill();
        startTimer (100);
        if (onStateChanged)
            onStateChanged();
    }

    void Session::restartQemu()
    {
        if (qemuState.load() == QemuState::stopped)
        {
            startQemu();
            return;
        }
        restartPending = true;
        stopQemu();
    }

    void Session::timerCallback()
    {
        // Escalate to a kill if QEMU ignores the quit command.
        if (qemuState.load() == QemuState::stopping
            && juce::Time::getMillisecondCounter() - stopRequestedAt > (juce::uint32) config.getInt ("host.qemu_quit_timeout_ms", 3000))
        {
            log.add ("host", "QEMU did not quit in time, killing it");
            qemu.kill();
            stopTimer();
        }
    }

    void Session::processExited (int code)
    {
        stopTimer();
        rfb.stop();
        qmp.stop();
        framebuffer.serial++;
        qemuState = QemuState::stopped;
        log.add ("host", "QEMU exited with code " + juce::String (code));
        if (onStateChanged)
            onStateChanged();

        if (restartPending)
        {
            restartPending = false;
            startQemu();
        }
    }
}
