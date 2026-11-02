// KickEmu: host application of the KICK emulator.
//
// Starts the UART/CV sockets and the MIDI bridge, launches QEMU from the
// configuration and shows the device. With --headless it runs without a
// window and logs to stdout.

#include "app/AppSettings.h"
#include "app/AutomationTools.h"
#include "automation/AutomationSocket.h"
#include "automation/McpServer.h"
#include "emu/NetworkConfig.h"
#include "app/CommandLine.h"
#include "app/MainComponent.h"
#include "app/Session.h"

#include <BuildInfo.h>
#include <KickEmuResources.h>
#include <cstdio>
#include <thread>

namespace kickemu
{
    /** Keeps the device area at its aspect ratio while the user resizes the window. */
    class DeviceFitConstrainer : public juce::ComponentBoundsConstrainer
    {
    public:
        WindowFit fit;

        void checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous,
                          const juce::Rectangle<int>& limits, bool top, bool left, bool bottom, bool right) override
        {
            if (fit.isActive())
            {
                int edges = WindowFit::none;
                if (top) edges |= WindowFit::top;
                if (left) edges |= WindowFit::left;
                if (bottom) edges |= WindowFit::bottom;
                if (right) edges |= WindowFit::right;
                bounds = fit.constrain (bounds, edges);
            }
            ComponentBoundsConstrainer::checkBounds (bounds, previous, limits, top, left, bottom, right);
        }
    };

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (Session& session, AppSettings& s)
            : DocumentWindow ("KickEmu", juce::Colour (0xff121214), DocumentWindow::allButtons), settings (s)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent (session, settings);
            content->onRequestSize = [this] (int w, int h)
            {
                constrainer.fit = {};   // a fixed scale sizes the window itself
                if (! isFullScreen())
                    setSize (w, h);
            };
            content->onWindowFitChanged = [this, content] { applyFit (content->getWindowFit()); };
            setContentOwned (content, false);
            constrainer.setMinimumSize (480, 400);
            setConstrainer (&constrainer);
            setResizable (true, false);

            auto state = settings.getString ("windowState");
            if (state.isEmpty() || ! restoreWindowStateFromString (state))
                centreWithSize (1180, 1000);
            setVisible (true);
            applyFit (content->getWindowFit());
        }

        /** Locks the window to the device shape (or releases it) and reshapes it once. */
        void applyFit (const WindowFit& fit)
        {
            constrainer.fit = fit;
            if (! fit.isActive() || isFullScreen() || isMinimised())
                return;
            const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
            if (display != nullptr)
                setBounds (fit.snap (getBounds(), display->userArea));
        }

        ~MainWindow() override
        {
            settings.set ("windowState", getWindowStateAsString());
            clearContentComponent();
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        AppSettings& settings;
        DeviceFitConstrainer constrainer;
    };

    class KickEmuApplication : public juce::JUCEApplication
    {
    public:
        const juce::String getApplicationName() override { return "KickEmu"; }
        const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
        bool moreThanOneInstanceAllowed() override { return true; }

        void initialise (const juce::String&) override
        {
            cl = CommandLine::parse (getCommandLineParameterArray());
            if (cl.error.isNotEmpty())
            {
                std::fprintf (stderr, "%s\n\n%s", cl.error.toRawUTF8(), CommandLine::helpText().toRawUTF8());
                setApplicationReturnValue (2);
                quit();
                return;
            }
            if (cl.help || cl.version)
            {
                std::printf ("%s", (cl.help ? CommandLine::helpText() : CommandLine::versionText() + "\n").toRawUTF8());
                quit();
                return;
            }

            // Configuration: file, profile (CLI or remembered), overrides.
            config = std::make_unique<Config>();
            auto file = cl.configFile != juce::File() ? cl.configFile : Config::findDefaultConfigFile();
            auto result = config->load (file);
            auto profile = cl.profile;
            if (result.wasOk() && profile.isNotEmpty())
                result = config->applyProfile (profile);
            if (result.wasOk() && cl.runDir.isNotEmpty())
                result = config->applyOverride ("run_dir=" + cl.runDir);
            for (auto& o : cl.overrides)
                if (result.wasOk())
                    result = config->applyOverride (o);
            // Headless runs make no sound unless a backend was chosen explicitly.
            if (result.wasOk() && cl.headless && config->getString ("qemu.audiodev") == "auto")
                result = config->applyOverride ("qemu.audiodev=none");
            if (result.failed())
            {
                fail (result.getErrorMessage(), cl.headless);
                return;
            }

            if (cl.printCommand)
            {
                auto cmd = QemuCommandLine::build (*config);
                for (auto& w : cmd.warnings)
                    std::fprintf (stderr, "warning: %s\n", w.toRawUTF8());
                if (! cmd.ok())
                    std::fprintf (stderr, "error: %s\n", cmd.error.toRawUTF8());
                std::printf ("%s\n", cmd.toShellString().toRawUTF8());
                setApplicationReturnValue (cmd.ok() ? 0 : 1);
                quit();
                return;
            }

            settings = std::make_unique<AppSettings>();
            // The network chosen on the LINK port, unless the command line sets one.
            if (auto net = settings->getString ("network"); net.isNotEmpty() && ! cl.headless
                && ! cl.overrides.joinIntoString (" ").contains ("qemu.net"))
                NetworkConfig::fromVar (juce::JSON::parse (net), NetworkConfig::fromConfig (*config)).toConfig (*config);
            // The audio backend picked on the panel's audio jacks, unless set on the command line.
            if (auto dev = settings->getString ("audiodev"); dev.isNotEmpty() && ! cl.headless
                && ! cl.overrides.joinIntoString (" ").contains ("qemu.audiodev"))
                config->set ("qemu.audiodev", dev);
            session = std::make_unique<Session> (*config);
            // Under --mcp stdout carries the protocol, so the log goes to stderr.
            session->log.echo = cl.mcp ? stderr : cl.headless ? stdout : nullptr;
            session->log.add ("host", CommandLine::versionText());
            session->log.add ("host", "Config " + config->getFile().getFullPathName()
                                      + (config->getActiveProfile().isNotEmpty() ? " (profile " + config->getActiveProfile() + ")" : juce::String()));

            if (auto r = session->startBridges(); r.failed())
            {
                fail (r.getErrorMessage(), cl.headless);
                return;
            }

            startAutomation();

            // Restore the MIDI device bindings.
            session->midi.setInputDevice (settings->getString ("midiInputDevice"));
            session->midi.setOutputDevice (settings->getString ("midiOutputDevice"));
            for (int ch = 0; ch < session->cv.getNumChannels(); ++ch)
                if (settings->file().containsKey ("cvIn" + juce::String (ch)))
                    session->cv.setInputVolts (ch, (float) settings->getDouble ("cvIn" + juce::String (ch), 0.0));

            if (cl.mcp)
            {
                // The agent decides when to quit: closing stdin ends the server.
                mcpThread = std::thread ([this]
                {
                    mcp->serve (stdin, stdout, mcpStop);
                    mcpFinished = true;
                    juce::MessageManager::callAsync ([] { juce::JUCEApplication::quit(); });
                });
                if (! cl.noAutostart && config->getBool ("host.autostart", true))
                    session->startQemu();
                return;
            }

            if (cl.headless)
            {
                // Mirror the console to stdout and exit with QEMU.
                session->onConsoleOutput = [] (const juce::uint8* d, size_t n) { std::fwrite (d, 1, n, stdout); std::fflush (stdout); };
                session->onStateChanged = [this]
                {
                    if (session->getQemuState() == Session::QemuState::stopped)
                    {
                        if (cl.expect.isNotEmpty() && ! expectMatched)
                        {
                            std::fprintf (stderr, "KickEmu: QEMU exited before the console printed /%s/\n", cl.expect.toRawUTF8());
                            setApplicationReturnValue (4);
                        }
                        quit();
                    }
                };
                if (! session->startQemu())
                {
                    setApplicationReturnValue (1);
                    quit();
                    return;
                }
                if (cl.expect.isNotEmpty())
                    startExpect();
                return;
            }

            if (cl.screenshotView.isNotEmpty())
                settings->set ("view", juce::StringArray { "top", "rear", "front" }.indexOf (cl.screenshotView));

            if (cl.render != juce::File())
            {
                // The real UI, never added to the desktop: it paints into an image only.
                offscreen = std::make_unique<MainComponent> (*session, *settings);
                offscreen->setSize (1180, 1000);
                if (cl.presentation)
                    offscreen->setPresentation (true);
                if (! cl.noAutostart && config->getBool ("host.autostart", true))
                    session->startQemu();
                juce::Timer::callAfterDelay (cl.screenshotDelayMs, [this, target = cl.render]
                {
                    for (int i = 0; i < 12; ++i)   // let LED fades reach their targets
                    {
                        offscreen->tickDevice();
                        juce::Thread::sleep (16);
                    }
                    auto image = offscreen->createComponentSnapshot (offscreen->getLocalBounds(), true, 2.0f);
                    target.deleteFile();
                    juce::FileOutputStream out (target);
                    juce::PNGImageFormat().writeImageToStream (image, out);
                    std::printf ("render: %s\n", target.getFullPathName().toRawUTF8());
                    quit();
                });
                return;
            }
            window = std::make_unique<MainWindow> (*session, *settings);
            if (cl.presentation)
                if (auto* main = dynamic_cast<MainComponent*> (window->getContentComponent()))
                    main->setPresentation (true);
            if (cl.screenshot != juce::File())
            {
                juce::Timer::callAfterDelay (cl.screenshotDelayMs, [this, target = cl.screenshot]
                {
                    if (auto* content = window != nullptr ? window->getContentComponent() : nullptr)
                    {
                        auto image = content->createComponentSnapshot (content->getLocalBounds(), true, 2.0f);
                        target.deleteFile();
                        juce::FileOutputStream out (target);
                        juce::PNGImageFormat().writeImageToStream (image, out);
                        std::printf ("screenshot: %s\n", target.getFullPathName().toRawUTF8());
                    }
                    quit();
                });
            }
            if (! cl.noAutostart && config->getBool ("host.autostart", true))
                session->startQemu();
        }

        void shutdown() override
        {
            window.reset();
            offscreen.reset();
            expectStop = true;
            if (expectThread.joinable())
                expectThread.join();
            automationSocket.reset();
            session.reset();
            if (mcpThread.joinable())
            {
                if (mcpFinished.load())
                {
                    mcpThread.join();
                }
                else
                {
                    // The reader is blocked on stdin and cannot be woken portably. QEMU is
                    // already down (session reset above): leave without unwinding it.
                    std::fflush (nullptr);
                    std::_Exit (getApplicationReturnValue());
                }
            }
            if (settings != nullptr)
                settings->file().saveIfNeeded();
            settings.reset();
        }

        void systemRequestedQuit() override { quit(); }

    private:
        void startAutomation()
        {
            juce::String layoutJson (KickEmuResources::layout_json, (size_t) KickEmuResources::layout_jsonSize);
            if (auto file = config->getPath ("host.ui.layout_file"); file.existsAsFile())
                layoutJson = file.loadFileAsString();
            if (auto r = layout.parse (layoutJson); r.failed())
                session->log.add ("host", r.getErrorMessage());

            tools = std::make_unique<AutomationTools> (*session, layout);
            mcp = std::make_unique<McpServer> (tools->registry(), "kickemu", buildinfo::version, AutomationTools::instructions());
            automationSocket = std::make_unique<AutomationSocket> (*mcp);
            juce::String error;
            auto path = config->getSocketPath ("automation");
            if (automationSocket->start (path, error))
                session->log.add ("host", "Automation (MCP) on " + path.getFullPathName());
            else
                session->log.add ("host", "Automation socket unavailable: " + error);
        }

        void startExpect()
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) (cl.timeoutSeconds > 0 ? cl.timeoutSeconds * 1000 : 0x7fffffff);
            expectThread = std::thread ([this, deadline]
            {
                while (! expectStop.load())
                {
                    juce::String error;
                    auto m = session->consoleText.waitFor (cl.expect, 0, 200, error);
                    if (error.isNotEmpty() || m.found)
                    {
                        expectMatched = m.found;
                        const int code = m.found ? 0 : 2;
                        if (m.found)
                            std::fprintf (stderr, "\nKickEmu: matched /%s/: %s\n", cl.expect.toRawUTF8(), m.context.toRawUTF8());
                        else
                            std::fprintf (stderr, "KickEmu: %s\n", error.toRawUTF8());
                        juce::MessageManager::callAsync ([this, code] { finishHeadless (code); });
                        return;
                    }
                    if (juce::Time::getMillisecondCounter() > deadline)
                    {
                        std::fprintf (stderr, "\nKickEmu: timeout after %d s waiting for /%s/\n", cl.timeoutSeconds, cl.expect.toRawUTF8());
                        juce::MessageManager::callAsync ([this] { finishHeadless (3); });
                        return;
                    }
                }
            });
        }

        void finishHeadless (int code)
        {
            setApplicationReturnValue (code);
            session->onStateChanged = [this] { if (session->getQemuState() == Session::QemuState::stopped) quit(); };
            if (session->getQemuState() == Session::QemuState::stopped)
                quit();
            else
                session->stopQemu();
        }

        void fail (const juce::String& message, bool headless)
        {
            std::fprintf (stderr, "KickEmu: %s\n", message.toRawUTF8());
            setApplicationReturnValue (1);
            if (headless)
            {
                quit();
                return;
            }
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                              .withTitle ("KickEmu cannot start").withMessage (message).withButton ("Quit"),
                                          [] (int) { juce::JUCEApplication::quit(); });
        }

        std::unique_ptr<Config> config;
        std::unique_ptr<AppSettings> settings;
        std::unique_ptr<Session> session;
        std::unique_ptr<MainWindow> window;
        std::unique_ptr<MainComponent> offscreen;

        CommandLine cl;
        Layout layout;
        std::unique_ptr<AutomationTools> tools;
        std::unique_ptr<McpServer> mcp;
        std::unique_ptr<AutomationSocket> automationSocket;
        std::thread mcpThread, expectThread;
        std::atomic<bool> mcpStop { false }, mcpFinished { false }, expectStop { false }, expectMatched { false };
    };
}

START_JUCE_APPLICATION (kickemu::KickEmuApplication)
