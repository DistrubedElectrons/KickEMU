#pragma once

#include "config/WindowFit.h"

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
    /**
        The way out of presentation mode for the mouse: a pill at the top edge that
        fades in when the pointer comes near, and briefly when the mode starts.
    */
    class PresentationExit : public juce::Component
    {
    public:
        explicit PresentationExit (const Theme& t) : theme (t) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

        std::function<void()> onClick;

        void paint (juce::Graphics& g) override;
        void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (onClick && getLocalBounds().contains (e.getPosition()))
                onClick();
        }

        float level = 0.0f, target = 0.0f;  // fade 0..1
        juce::uint32 holdUntil = 0;         // ms counter: stay visible until then

    private:
        const Theme& theme;
        bool hover = false;
    };

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

        /** Window shape that shows the device without bars (inactive unless Fit and top view). */
        WindowFit getWindowFit() const;

        /** Called when that shape changes: Fit chosen, view, presentation or side panel toggled. */
        std::function<void()> onWindowFitChanged;

        void setPresentation (bool on);
        bool isPresentation() const { return presentation; }
        void saveSnapshot();
        /** Steps the panel animation once. The view animates on vblank, which only
            exists for a component on screen; offscreen rendering drives it here. */
        void tickDevice();
        void setSidePanelOpen (bool open, bool animate = true);

    private:
        void timerCallback() override;
        void updateStatus();
        void applyScaleSelection (int itemId);
        void layoutChildren();
        void showHint (const juce::String& text);
        void showConnectorMenu (const juce::String& connectorId);
        void chooseFile (const juce::String& title, const juce::String& patterns, bool save, std::function<void (const juce::File&)> then);
        /** Hint for an asynchronous peripheral operation, on the message thread. */
        std::function<void (const juce::String&)> reportResult (const juce::String& success);
        void updatePresentationExit();
        void mouseDown (const juce::MouseEvent& e) override;

        Session& session;
        AppSettings& settings;
        Theme theme;
        Layout layout;
        std::unique_ptr<ChromeLookAndFeel> lookAndFeel;
        std::unique_ptr<PanelContext> panelContext;
        std::unique_ptr<DeviceView> device;
        TitleBar titleBar;
        PresentationExit presentationExit { theme };
        std::unique_ptr<SidePanel> sidePanel;
        juce::TooltipWindow tooltips { this, 600 };

        bool presentation = false, sideOpen = false;
        float sideFraction = 0.0f, sideTarget = 0.0f;   // slide animation 0..1
        int sideWidth = 460;
        juce::Colour presentationBackground;
        juce::String hint;
        juce::uint32 hintTime = 0;
        juce::Rectangle<int> statusLine;
        std::unique_ptr<juce::FileChooser> fileChooser;
    };
}
