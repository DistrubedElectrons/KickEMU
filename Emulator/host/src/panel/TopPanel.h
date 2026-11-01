#pragma once

#include "config/Layout.h"
#include "panel/ButtonComponent.h"
#include "panel/JogWheelComponent.h"
#include "panel/KnobComponent.h"
#include "panel/PadComponent.h"
#include "panel/ScreenComponent.h"
#include "panel/SliderComponent.h"

namespace kickemu
{
    /**
        The top view of the KICK. Everything that never changes (enclosure,
        bevel, printed text, recesses, bezel, key wells) is rendered once per
        scale into a cached image; the controls on top are child components that
        repaint only when their state changes.
    */
    class TopPanel : public juce::Component
    {
    public:
        TopPanel (PanelContext& context, const Layout& layout, SharedFramebuffer& framebuffer, QmpClient& qmp);

        /** Device size in mm plus the shadow margin around it. */
        juce::Rectangle<float> getSizeMm() const;
        float getMarginMm() const { return shadowMarginMm; }

        /** Lays out everything for a scale in px per mm. */
        void setScale (float pxPerMm);
        float getScale() const { return scale; }

        /** Called at display refresh rate. */
        void animate (double now, float dtMs);

        void paint (juce::Graphics& g) override;

        /** Finds the control mapped to a key description (layout "key"). */
        PanelControl* controlForKey (const juce::String& key) const;
        const std::vector<PanelControl*>& getControls() const { return controls; }
        JogWheelComponent* getJog() const { return jog; }
        ScreenComponent* getScreen() const { return screen; }
        KnobComponent* getPot (const juce::String& id) const;
        KnobComponent* getEncoder (int index) const;

        /** Renders the panel (static layer + controls) into an image at the given pixel scale. */
        juce::Image snapshot (float pixelScale);

    private:
        void renderStatic (juce::Graphics& g) const;
        void drawEnclosure (juce::Graphics& g) const;
        void drawDecor (juce::Graphics& g, const LayoutDecor& d) const;
        void drawWells (juce::Graphics& g) const;
        void drawTexts (juce::Graphics& g) const;
        juce::Rectangle<float> toPx (juce::Rectangle<float> mm) const;
        juce::Point<float> toPx (juce::Point<float> mm) const;

        PanelContext& ctx;
        const Layout& layout;
        float scale = 3.0f, shadowMarginMm = 9.0f;
        juce::Image staticLayer;
        float staticPixelScale = 0.0f;

        juce::OwnedArray<juce::Component> owned;
        std::vector<PanelControl*> controls;
        JogWheelComponent* jog = nullptr;
        ScreenComponent* screen = nullptr;
    };
}
