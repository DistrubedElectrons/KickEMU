#pragma once

#include "panel/RearPanel.h"
#include "panel/TopPanel.h"

namespace kickemu
{
    /**
        Hosts the three views of the device (top, rear, front), scales them to
        the available space keeping the aspect ratio, drives the frame-rate
        animation and maps the computer keyboard to the controls.
    */
    class DeviceView : public juce::Component
    {
    public:
        enum class View { top, rear, front };

        DeviceView (PanelContext& context, const Layout& layout, SharedFramebuffer& framebuffer, QmpClient& qmp, IoStatus& io);
        ~DeviceView() override;

        void setView (View v);
        View getView() const { return view; }

        /** 0 = fit to the window, otherwise a fixed percentage (100 = host.ui.pixels_per_mm_at_100). */
        void setScalePercent (int percent);
        int getScalePercent() const { return scalePercent; }

        /** Width / height of the top view in Fit mode, 0 when the window should stay free
            (fixed scale, or the long rear/front strips). */
        double fitAspect() const;

        /** Size the view needs at a fixed percentage (used to size the window). */
        juce::Rectangle<int> preferredSize (int percent) const;

        TopPanel& getTop() { return *top; }

        /** PNG-ready image of the current view at a pixel scale (e.g. 2 for Retina-sized output). */
        juce::Image snapshot (float pixelScale);

        void resized() override;
        bool keyStateChanged (bool isKeyDown) override;
        bool keyPressed (const juce::KeyPress& key) override;
        void modifierKeysChanged (const juce::ModifierKeys& mods) override;
        void focusLost (FocusChangeType) override;

        /** Text listing the keyboard map, for the help panel. */
        juce::StringArray describeKeyboard() const;

    private:
        void tick();
        void releaseAllKeys();
        static int keyCodeFor (const juce::String& name);

        PanelContext& ctx;
        const Layout& layout;
        std::unique_ptr<TopPanel> top;
        std::unique_ptr<RearPanel> rear;
        std::unique_ptr<FrontPanel> front;
        View view = View::top;
        int scalePercent = 0;
        float pixelsPerMmAt100 = 3.2f;

        struct KeyBinding
        {
            int code;
            PanelControl* control;
            bool down = false;
        };
        std::vector<KeyBinding> bindings;
        int jogPushKey = 0, jogCcwKey = 0, jogCwKey = 0;
        bool jogPushDown = false, shiftKeyDown = false;

        juce::VBlankAttachment vblank;
        double lastTick = 0.0;
    };
}
