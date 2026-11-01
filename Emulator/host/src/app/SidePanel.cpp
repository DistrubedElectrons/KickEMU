#include "app/SidePanel.h"

namespace kickemu
{
    SidePanel::SidePanel (Session& session, AppSettings& settings, const Theme& t, juce::StringArray keyboardMap)
        : theme (t),
          consoleView (session.console, t, (float) session.config.getDouble ("host.ui.terminal_font_size", 13.0)),
          uart2View (session.uart2, t, (float) session.config.getDouble ("host.ui.terminal_font_size", 13.0)),
          logView (session.log, t),
          ioView (session, settings, t),
          settingsView (session, settings, t, std::move (keyboardMap))
    {
        logView.getCommandLine = [&session] { return session.getLastCommandLine(); };

        const auto bg = theme.chrome;
        tabs.setTabBarDepth (34);
        tabs.setOutline (0);
        tabs.addTab ("Console", bg, &consoleView, false);
        tabs.addTab ("UART2", bg, &uart2View, false);
        tabs.addTab ("Log", bg, &logView, false);
        tabs.addTab ("I/O", bg, &ioView, false);
        tabs.addTab ("Settings", bg, &settingsView, false);
        tabs.setCurrentTabIndex (juce::jlimit (0, 4, settings.getInt ("sidePanelTab", 0)), false);
        addAndMakeVisible (tabs);
    }

    void SidePanel::paint (juce::Graphics& g)
    {
        g.fillAll (theme.chrome);
        g.setColour (theme.chromeLine);
        g.fillRect (getLocalBounds().removeFromLeft (1));
    }

    void SidePanel::resized() { tabs.setBounds (getLocalBounds().withTrimmedLeft (1)); }
}
