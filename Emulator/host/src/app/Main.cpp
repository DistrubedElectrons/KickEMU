// KickEmu: host application of the KICK emulator.
//
// Starts the UART/CV sockets and the MIDI bridge, launches QEMU from the
// configuration and shows the device. With --headless it runs without a
// window and logs to stdout.

#include "app/AppSettings.h"
#include "app/CommandLine.h"
#include "app/MainComponent.h"
#include "app/Session.h"

#include <cstdio>

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
            auto cl = CommandLine::parse (getCommandLineParameterArray());
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
            for (auto& o : cl.overrides)
                if (result.wasOk())
                    result = config->applyOverride (o);
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
            session = std::make_unique<Session> (*config);
            session->log.echoToStdout = cl.headless;
            session->log.add ("host", CommandLine::versionText());
            session->log.add ("host", "Config " + config->getFile().getFullPathName()
                                      + (config->getActiveProfile().isNotEmpty() ? " (profile " + config->getActiveProfile() + ")" : juce::String()));

            if (auto r = session->startBridges(); r.failed())
            {
                fail (r.getErrorMessage(), cl.headless);
                return;
            }

            // Restore the MIDI device bindings.
            session->midi.setInputDevice (settings->getString ("midiInputDevice"));
            session->midi.setOutputDevice (settings->getString ("midiOutputDevice"));
            for (int ch = 0; ch < session->cv.getNumChannels(); ++ch)
                if (settings->file().containsKey ("cvIn" + juce::String (ch)))
                    session->cv.setInputVolts (ch, (float) settings->getDouble ("cvIn" + juce::String (ch), 0.0));

            if (cl.headless)
            {
                // Mirror the console to stdout and exit with QEMU.
                session->console.onRawOutput = [] (const juce::uint8* d, size_t n) { std::fwrite (d, 1, n, stdout); std::fflush (stdout); };
                session->onStateChanged = [this]
                {
                    if (session->getQemuState() == Session::QemuState::stopped)
                        quit();
                };
                if (! session->startQemu())
                {
                    setApplicationReturnValue (1);
                    quit();
                }
                return;
            }

            if (cl.screenshotView.isNotEmpty())
                settings->set ("view", juce::StringArray { "top", "rear", "front" }.indexOf (cl.screenshotView));
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
            session.reset();
            if (settings != nullptr)
                settings->file().saveIfNeeded();
            settings.reset();
        }

        void systemRequestedQuit() override { quit(); }

    private:
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
    };
}

START_JUCE_APPLICATION (kickemu::KickEmuApplication)
