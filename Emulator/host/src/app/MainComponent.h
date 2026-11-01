#pragma once

#include "app/AppSettings.h"
#include "app/Session.h"
#include "app/SidePanel.h"
#include "app/TitleBar.h"
#include "panel/DeviceView.h"

namespace kickemu
{
    /** Dark look for the app chrome (combo boxes, tabs, sliders, tooltips). */
    class ChromeLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit ChromeLookAndFeel (const Theme& theme);
        juce::Font getComboBoxFont (juce::ComboBox&) override { return theme.uiFont (12.5f); }
        juce::Font getTextButtonFont (juce::TextButton&, int) override { return theme.uiFont (12.5f); }
        juce::Font getTabButtonFont (juce::TabBarButton&, float) override { return theme.uiFont (12.5f, true); }
        void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
        void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
        int getTabButtonBestWidth (juce::TabBarButton&, int) override { return 80; }

    private:
        const Theme& theme;
    };

    /**
        The window content: title bar, the device centred on a dark background,
        a slide-out side panel, and a slim status line. Presentation mode hides
        everything but the device.
    */
    class MainComponent : public juce::Component, private juce::Timer
    {
    public:
        MainComponent (Session& session, AppSettings& settings);
        ~MainComponent() override;

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

        /** Asks the window to resize so the device fits a scale preset. */
        std::function<void (int width, int height)> onRequestSize;

        void setPresentation (bool on);
        bool isPresentation() const { return presentation; }
        void saveSnapshot();
        void setSidePanelOpen (bool open, bool animate = true);

    private:
        void timerCallback() override;
        void updateStatus();
        void applyScaleSelection (int itemId);
        void layoutChildren();
        void showHint (const juce::String& text);

        Session& session;
        AppSettings& settings;
        Theme theme;
        Layout layout;
        std::unique_ptr<ChromeLookAndFeel> lookAndFeel;
        std::unique_ptr<PanelContext> panelContext;
        std::unique_ptr<DeviceView> device;
        TitleBar titleBar;
        std::unique_ptr<SidePanel> sidePanel;
        juce::TooltipWindow tooltips { this, 600 };

        bool presentation = false, sideOpen = false;
        float sideFraction = 0.0f, sideTarget = 0.0f;   // slide animation 0..1
        int sideWidth = 460;
        juce::Colour presentationBackground;
        juce::String hint;
        juce::uint32 hintTime = 0;
        juce::Rectangle<int> statusLine;
    };
}
