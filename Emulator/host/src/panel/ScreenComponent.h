#pragma once

#include "emu/QmpClient.h"
#include "emu/RfbClient.h"
#include "panel/PanelControl.h"

namespace kickemu
{
    /**
        The 7" display: the guest framebuffer (from the RFB client) scaled into
        the active area with high-quality resampling, under a faint glass
        reflection. Mouse and trackpad input on it becomes QMP multitouch (mtt)
        and absolute (abs) events on the 0..0x7fff axes; each pointer source is a
        separate contact, and holding the pinch modifier adds a second finger
        mirrored around the screen centre.
    */
    class ScreenComponent : public PanelControl
    {
    public:
        ScreenComponent (PanelContext& context, const LayoutControl& definition, SharedFramebuffer& framebuffer, QmpClient& qmp);

        bool animate (double now, float dtMs) override;
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

        /** Status text shown when there is no picture (e.g. "QEMU stopped"). */
        void setStatus (const juce::String& text);

    private:
        void rebuild() override;
        juce::Rectangle<float> imageArea() const;
        juce::Point<int> toAxes (juce::Point<float> p) const;

        enum class Phase { begin, update, end };
        void sendContact (int slot, Phase phase, juce::Point<int> axes);
        juce::var mtt (const char* type, int slot, int trackingId, const char* axis, int value) const;

        SharedFramebuffer& fb;
        QmpClient& qmp;
        juce::uint32 lastSerial = 0;
        int lastBacklight = 1000;
        bool lastConnected = false;
        juce::String status;
        juce::Image glare;

        int absMax = 0x7fff, maxContacts = 5;
        bool keepAspect = true, sendAbs = true;
        juce::ModifierKeys pinchModifier = juce::ModifierKeys::altModifier;

        struct Contact { bool active = false; int trackingId = -1; juce::Point<int> axes; };
        std::array<Contact, 10> contacts {};
        int nextTrackingId = 0;
        bool pinching = false;
    };
}
