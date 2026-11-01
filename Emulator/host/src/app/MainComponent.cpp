#include "app/MainComponent.h"

#include <KickEmuResources.h>

namespace kickemu
{
    namespace
    {
        constexpr int titleHeight = 44;
        constexpr int statusHeight = 24;
        constexpr int fitItemId = 1000;
    }

    // ------------------------------------------------------------------------------------

    ChromeLookAndFeel::ChromeLookAndFeel (const Theme& t) : theme (t)
    {
        auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::windowBackground, t.chrome);
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::widgetBackground, juce::Colour (0xff222226));
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::menuBackground, juce::Colour (0xff1e1e22));
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::outline, t.chromeLine);
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultText, t.chromeText);
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultFill, t.accent);
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedText, juce::Colours::white);
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::highlightedFill, t.accent.withAlpha (0.35f));
        scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::menuText, t.chromeText);
        setColourScheme (scheme);
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xf0202024));
        setColour (juce::TooltipWindow::textColourId, t.chromeText);
        setColour (juce::TooltipWindow::outlineColourId, t.chromeLine);
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff222226));
        setColour (juce::ComboBox::outlineColourId, t.chromeLine);
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff26262b));
        setColour (juce::TextButton::textColourOffId, t.chromeText);
        setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2a2a30));
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxTextColourId, t.chromeText);
        setDefaultSansSerifTypefaceName (t.uiFontName);
    }

    void ChromeLookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
    {
        auto r = b.getLocalBounds().toFloat();
        const bool front = b.isFrontTab();
        if (front)
        {
            g.setColour (theme.accent);
            g.fillRect (r.removeFromBottom (2.0f).reduced (10.0f, 0.0f));
        }
        g.setColour (front ? theme.chromeText : (over ? theme.chromeText.withAlpha (0.85f) : theme.chromeDim));
        g.setFont (theme.uiFont (12.5f, front));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    // ------------------------------------------------------------------------------------

    MainComponent::MainComponent (Session& s, AppSettings& st)
        : session (s), settings (st), titleBar (theme)
    {
        auto& config = session.config;

        // Layout and device theme: embedded layout.json unless the config points elsewhere.
        juce::String layoutJson (KickEmuResources::layout_json, (size_t) KickEmuResources::layout_jsonSize);
        if (auto file = config.getPath ("host.ui.layout_file"); file.existsAsFile())
            layoutJson = file.loadFileAsString();
        if (auto r = layout.parse (layoutJson); r.failed())
            session.log.add ("host", r.getErrorMessage());
        theme.load (juce::JSON::parse (layoutJson)["theme"]);
        for (auto& c : config.getStringArray ("host.pad_palette"))
            theme.padPalette.add (Theme::parseColour (c, juce::Colours::transparentBlack));

        lookAndFeel = std::make_unique<ChromeLookAndFeel> (theme);
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        panelContext = std::make_unique<PanelContext> (theme, session.panel, session.mcu.protocol(), config);
        panelContext->onHint = [this] (const juce::String& text) { showHint (text); };
        device = std::make_unique<DeviceView> (*panelContext, layout, session.framebuffer, session.qmp, session.io);
        sidePanel = std::make_unique<SidePanel> (session, settings, theme, device->describeKeyboard());

        addAndMakeVisible (*device);
        addAndMakeVisible (titleBar);
        addChildComponent (presentationExit);
        presentationExit.onClick = [this] { setPresentation (false); };
        // Right clicks anywhere (the device included) reach mouseDown for the context menu
        addMouseListener (this, true);
        addChildComponent (*sidePanel);

        // Analog pots keep their position between runs.
        for (auto id : { "master", "rec" })
        {
            if (auto* pot = device->getTop().getPot (id))
            {
                pot->setPotValue ((float) settings.getDouble (juce::String ("pot_") + id, 0.5));
                pot->onPotChanged = [this, key = juce::String ("pot_") + id] (float v) { settings.set (key, v); };
            }
        }

        session.io.onConnectorClicked = [this] (const juce::String& id)
        {
            if (id.startsWith ("cv") || id.startsWith ("midi"))
            {
                setSidePanelOpen (true);
                sidePanel->showTab (SidePanel::io);
            }
        };

        // Title bar wiring.
        titleBar.views.onChange = [this] (int i)
        {
            device->setView ((DeviceView::View) i);
            settings.set ("view", i);
            if (onWindowFitChanged)
                onWindowFitChanged();
        };
        titleBar.power.onClick = [this]
        {
            if (session.getQemuState() == Session::QemuState::stopped)
                session.startQemu();
            else
                session.stopQemu();
            updateStatus();
        };
        titleBar.restart.onClick = [this] { session.restartQemu(); updateStatus(); };
        titleBar.snapshot.onClick = [this] { saveSnapshot(); };
        titleBar.presentation.onClick = [this] { setPresentation (true); };
        titleBar.panel.onClick = [this] { setSidePanelOpen (! sideOpen); };

        titleBar.scale.addItem ("Fit", fitItemId);
        for (auto p : config.getIntArray ("host.ui.scale_presets"))
            titleBar.scale.addItem (juce::String (p) + " %", p);
        titleBar.scale.onChange = [this] { applyScaleSelection (titleBar.scale.getSelectedId()); };

        sideWidth = settings.getInt ("sidePanelWidth", 460);
        presentationBackground = Theme::parseColour (settings.getString ("presentationBackground",
                                                                         config.getString ("host.ui.presentation_background", "#0c0c0e")),
                                                     theme.appBackground);
        sidePanel->getSettingsView().onPresentationColour = [this] (juce::Colour c) { presentationBackground = c; repaint(); };

        const int view = juce::jlimit (0, 2, settings.getInt ("view", 0));
        titleBar.views.setSelected (view);
        device->setView ((DeviceView::View) view);

        const int scaleId = settings.getInt ("scale", fitItemId);
        titleBar.scale.setSelectedId (scaleId == fitItemId || titleBar.scale.indexOfItemId (scaleId) >= 0 ? scaleId : fitItemId, juce::dontSendNotification);
        device->setScalePercent (titleBar.scale.getSelectedId() == fitItemId ? 0 : titleBar.scale.getSelectedId());

        setSidePanelOpen (settings.getBool ("sidePanelOpen", false), false);

        session.onStateChanged = [this] { updateStatus(); };
        setWantsKeyboardFocus (false);
        updateStatus();
        startTimerHz (30);
    }

    MainComponent::~MainComponent()
    {
        settings.set ("sidePanelTab", sidePanel->getCurrentTab());
        stopTimer();
        session.onStateChanged = nullptr;
        session.io.onConnectorClicked = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    WindowFit MainComponent::getWindowFit() const
    {
        WindowFit fit;
        fit.aspect = device->fitAspect();
        if (! presentation)
        {
            // Must match layoutChildren(): side panel, 8 px margin, title bar and status line
            fit.chromeWidth = (sideOpen ? sideWidth : 0) + 16;
            fit.chromeHeight = titleHeight + statusHeight + 16;
        }
        return fit;
    }

    void MainComponent::applyScaleSelection (int itemId)
    {
        settings.set ("scale", itemId);
        if (itemId == fitItemId)
        {
            device->setScalePercent (0);
            if (onWindowFitChanged)
                onWindowFitChanged();
            return;
        }
        // A fixed scale: grow or shrink the window around the device.
        device->setScalePercent (0);
        auto size = device->preferredSize (itemId);
        if (onRequestSize)
            onRequestSize (size.getWidth() + (sideOpen ? sideWidth : 0), size.getHeight() + titleHeight + statusHeight);
        device->setScalePercent (itemId);
        if (onWindowFitChanged)
            onWindowFitChanged();   // releases the aspect lock
    }

    void MainComponent::paint (juce::Graphics& g)
    {
        if (presentation)
        {
            g.fillAll (presentationBackground);
            return;
        }

        // Dark backdrop with a soft vignette so the device reads as an object on a table.
        auto area = getLocalBounds().withTrimmedTop (titleHeight).toFloat();
        g.setColour (theme.appBackground);
        g.fillRect (area);
        auto c = area.getCentre();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d1d21), c.x, c.y - area.getHeight() * 0.1f,
                                                 theme.appBackground, c.x, area.getBottom() + area.getHeight() * 0.2f, true));
        g.fillRect (area);

        if (! statusLine.isEmpty())
        {
            g.setFont (theme.uiFont (11.5f));
            g.setColour (theme.chromeDim);
            const bool fresh = juce::Time::getMillisecondCounter() - hintTime < 4000;
            g.drawText (fresh ? hint : juce::String(), statusLine, juce::Justification::centredLeft);
            auto& p = session.panel;
            juce::String right;
            right << "UART1 " << (juce::int64) p.packetsIn.load() << " in / " << (juce::int64) p.packetsOut.load() << " out";
            if (p.bpm.load() > 0)
                right << "   " << p.bpm.load() << " BPM";
            g.drawText (right, statusLine, juce::Justification::centredRight);
        }
    }

    void MainComponent::layoutChildren()
    {
        auto r = getLocalBounds();
        if (presentation)
        {
            titleBar.setVisible (false);
            sidePanel->setVisible (false);
            statusLine = {};
            device->setBounds (r);
            presentationExit.setBounds (juce::Rectangle<int> (236, 34).withCentre ({ r.getCentreX(), r.getY() + 30 }));
            presentationExit.toFront (false);
            return;
        }
        presentationExit.setVisible (false);

        titleBar.setVisible (true);
        titleBar.setBounds (r.removeFromTop (titleHeight));

        const int visibleSide = juce::roundToInt ((float) sideWidth * sideFraction);
        sidePanel->setVisible (visibleSide > 0);
        if (visibleSide > 0)
            sidePanel->setBounds (r.getRight() - visibleSide, r.getY(), sideWidth, r.getHeight());
        r.removeFromRight (visibleSide);

        statusLine = r.removeFromBottom (statusHeight).reduced (14, 0);
        device->setBounds (r.reduced (8));
    }

    void MainComponent::resized() { layoutChildren(); }

    void MainComponent::setSidePanelOpen (bool open, bool animate)
    {
        sideOpen = open;
        sideTarget = open ? 1.0f : 0.0f;
        settings.set ("sidePanelOpen", open);
        titleBar.panel.setHighlighted (open);
        if (! animate)
        {
            sideFraction = sideTarget;
            layoutChildren();
        }
        if (onWindowFitChanged)
            onWindowFitChanged();
    }

    void MainComponent::setPresentation (bool on)
    {
        presentation = on;
        layoutChildren();
        if (onWindowFitChanged)
            onWindowFitChanged();
        repaint();
        device->grabKeyboardFocus();
        if (on)
        {
            // Show the way out for a moment, then only when the pointer nears the top
            presentationExit.holdUntil = juce::Time::getMillisecondCounter() + 3500;
            presentationExit.target = 1.0f;
        }
    }

    void MainComponent::saveSnapshot()
    {
        const float scale = (float) session.config.getDouble ("host.ui.snapshot_scale", 2.0);
        auto image = device->snapshot (scale);
        auto dir = session.config.resolvePath (session.config.getString ("host.ui.snapshot_dir", "~/Pictures/KickEmu"));
        dir.createDirectory();
        auto file = dir.getNonexistentChildFile ("KickEmu " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S"), ".png", false);
        juce::PNGImageFormat png;
        juce::FileOutputStream out (file);
        if (out.openedOk() && png.writeImageToStream (image, out))
        {
            session.log.add ("host", "Snapshot saved: " + file.getFullPathName());
            showHint ("Snapshot saved to " + file.getFullPathName());
        }
        else
        {
            showHint ("Could not write " + file.getFullPathName());
        }
    }

    bool MainComponent::keyPressed (const juce::KeyPress& key)
    {
        const auto mods = key.getModifiers();
        const auto code = key.getKeyCode();
        if (mods.isCommandDown())
        {
            if (code == 'P') { setPresentation (! presentation); return true; }
            if (code == 'S' && mods.isShiftDown()) { saveSnapshot(); return true; }
            if (code == 'L') { setSidePanelOpen (! sideOpen); return true; }
            if (code == 'R') { session.restartQemu(); return true; }
            if (code == '.') { session.stopQemu(); return true; }
            if (code == '0') { titleBar.scale.setSelectedId (fitItemId); return true; }
            if (code >= '1' && code <= '3')
            {
                titleBar.views.setSelected (code - '1');
                device->setView ((DeviceView::View) (code - '1'));
                settings.set ("view", code - '1');
                if (onWindowFitChanged)
                    onWindowFitChanged();
                return true;
            }
            if (code == juce::KeyPress::escapeKey && presentation) { setPresentation (false); return true; }
        }
        return false;
    }

    void MainComponent::showHint (const juce::String& text)
    {
        hint = text;
        hintTime = juce::Time::getMillisecondCounter();
        repaint (statusLine);
    }

    void MainComponent::updateStatus()
    {
        TitleBar::Status s;
        switch (session.getQemuState())
        {
            case Session::QemuState::running: s.qemu = "Running"; s.qemuColour = theme.ok; break;
            case Session::QemuState::stopping: s.qemu = "Stopping"; s.qemuColour = theme.warn; break;
            case Session::QemuState::stopped: s.qemu = "Stopped"; s.qemuColour = theme.chromeDim; break;
        }
        auto& c = session.config;
        s.detail = c.getString ("qemu.machine") + " / " + c.getString ("qemu.boot");
        if (c.getActiveProfile().isNotEmpty())
            s.detail << " (" << c.getActiveProfile() << ")";
        s.mcuLinked = session.panel.hostLinked.load();
        s.uartConnected = session.panel.uartConnected.load();
        s.displayConnected = session.framebuffer.connected.load();
        titleBar.setStatus (s);

        titleBar.power.setIcon (session.getQemuState() == Session::QemuState::stopped ? icons::play : icons::stop);
        titleBar.power.setTooltip (session.getQemuState() == Session::QemuState::stopped ? "Start QEMU" : "Stop QEMU (Cmd+.)");

        if (auto* screen = device->getTop().getScreen())
        {
            switch (session.getQemuState())
            {
                case Session::QemuState::stopped: screen->setStatus ("Powered off"); break;
                case Session::QemuState::stopping: screen->setStatus ("Shutting down"); break;
                case Session::QemuState::running: screen->setStatus (s.displayConnected ? juce::String() : "Waiting for display"); break;
            }
        }
    }

    void MainComponent::updatePresentationExit()
    {
        auto& p = presentationExit;
        if (! presentation)
        {
            p.level = p.target = 0.0f;
            p.setVisible (false);
            return;
        }
        const auto mouse = getMouseXYRelative();
        const bool near = getLocalBounds().contains (mouse) && mouse.y < 72;
        p.target = (near || juce::Time::getMillisecondCounter() < p.holdUntil) ? 1.0f : 0.0f;
        if (std::abs (p.level - p.target) < 0.01f)
            p.level = p.target;
        else
            p.level += (p.target - p.level) * 0.25f;
        p.setVisible (p.level > 0.01f);
        p.setAlpha (p.level);
    }

    void MainComponent::mouseDown (const juce::MouseEvent& e)
    {
        // Context menu on the window background or the device body (not on controls)
        if (! e.mods.isPopupMenu() || (e.eventComponent != this && e.eventComponent != device.get()))
            return;
        juce::PopupMenu menu;
        menu.addItem (1, presentation ? "Leave presentation mode" : "Presentation mode", true, false);
        menu.addItem (2, "Save snapshot");
        menu.addItem (3, "Fit window to device");
        menu.showMenuAsync (juce::PopupMenu::Options(), [this] (int id)
        {
            if (id == 1) setPresentation (! presentation);
            if (id == 2) saveSnapshot();
            if (id == 3) titleBar.scale.setSelectedId (fitItemId);
        });
    }

    void PresentationExit::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xe0141417));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (hover ? theme.accent : theme.chromeLine);
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
        g.setColour (hover ? juce::Colours::white : theme.chromeText);
        g.setFont (theme.uiFont (13.0f, true));
        g.drawText ("Leave presentation", r.withTrimmedRight (r.getWidth() * 0.32f), juce::Justification::centredRight);
        g.setColour (theme.chromeDim);
        g.setFont (theme.uiFont (12.0f));
        g.drawText ("Cmd+P", r.withTrimmedLeft (r.getWidth() * 0.72f), juce::Justification::centredLeft);
    }

    void MainComponent::timerCallback()
    {
        updatePresentationExit();

        // Side panel slide.
        if (std::abs (sideFraction - sideTarget) > 0.001f)
        {
            sideFraction += (sideTarget - sideFraction) * 0.35f;
            if (std::abs (sideFraction - sideTarget) < 0.01f)
                sideFraction = sideTarget;
            layoutChildren();
        }

        static int counter = 0;
        if (++counter % 6 == 0)
        {
            updateStatus();
            if (! presentation)
                repaint (statusLine);
        }
    }
}
