#pragma once

#include "app/LogView.h"
#include "app/SettingsView.h"
#include "app/TerminalView.h"

namespace kickemu
{
    /** The slide-out panel on the right: consoles, log, I/O and settings. */
    class SidePanel : public juce::Component
    {
    public:
        SidePanel (Session& session, AppSettings& settings, const Theme& theme, juce::StringArray keyboardMap);

        enum Tab { console = 0, uart2, log, io, settings };
        void showTab (Tab t) { tabs.setCurrentTabIndex ((int) t); }
        int getCurrentTab() const { return tabs.getCurrentTabIndex(); }

        void paint (juce::Graphics& g) override;
        void resized() override;

        SettingsView& getSettingsView() { return settingsView; }
        IoView& getIoView() { return ioView; }

    private:
        const Theme& theme;
        TerminalView consoleView, uart2View;
        LogView logView;
        IoView ioView;
        SettingsView settingsView;
        juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    };
}
