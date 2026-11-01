#pragma once

#include "config/Config.h"
#include "config/Layout.h"
#include "emu/McuProtocol.h"
#include "emu/PanelState.h"
#include "panel/Painting.h"
#include "panel/Theme.h"

namespace kickemu
{
    /**
        Shared context for every control on the panel: theme, live state, the
        MCU to send events to and a few UI settings. Owned by the TopPanel.
    */
    struct PanelContext
    {
        const Theme& theme;
        PanelState& state;
        McuProtocol& mcu;
        const Config& config;

        double blinkPeriodSeconds = 0.5;
        float fadeMs = 70.0f;
        bool shiftAlternates = true;
        bool buttonLedFromPalette = false;
        juce::String jogRingDefault = "connected";
        int velocityMin = 24, velocityMax = 127;
        int aftertouchIntervalMs = 30;
        int longPressMs = 0;
        float encoderPxPerStep = 6.0f;
        float jogDegreesPerStep = 15.0f;
        int wheelStepsPerNotch = 1;

        std::atomic<bool> shiftHeld { false };   // MCU-local SHIFT state, set by the SHIFT key
        juce::Image noise;                       // shared noise tile

        /** Called when a control wants to show a status hint (e.g. key + id). */
        std::function<void (const juce::String&)> onHint;

        PanelContext (const Theme& t, PanelState& s, McuProtocol& m, const Config& c);
    };

    /**
        Base class for one control. Geometry comes from layout.json in mm; the
        component is placed at (rect * pxPerMm) expanded by a margin so glows can
        bleed outside the control. animate() is called at display refresh rate and
        returns true when the control needs repainting.
    */
    class PanelControl : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        PanelControl (PanelContext& context, const LayoutControl& definition, float marginMm);

        /** Places the control for a scale (px per mm) and rebuilds cached images. */
        void place (float pxPerMm, float pixelScale);

        /** Advances fades/animations; returns true if a repaint is needed. */
        virtual bool animate (double nowSeconds, float dtMs) = 0;

        /** Keyboard mapping: press/release as if clicked. */
        virtual void setKeyDown (bool down) { juce::ignoreUnused (down); }

        const LayoutControl& getDefinition() const { return def; }

        /** Key shortcut text for tooltips (e.g. "Key: Q"). */
        juce::String keyHint() const;

    protected:
        /** Rebuild caches after a scale change. `body` is already updated. */
        virtual void rebuild() {}

        /** Body rectangle (the control itself, without the glow margin), in local coords. */
        juce::Rectangle<float> body;
        float pxPerMm = 1.0f, pixelScale = 1.0f, margin = 0.0f;

        float mm (float v) const { return v * pxPerMm; }

        /** First-order approach to a target; tauMs is the time constant. */
        static float approach (float current, float target, float dtMs, float tauMs);

        PanelContext& ctx;
        const LayoutControl& def;
        float marginMm;
    };
}
