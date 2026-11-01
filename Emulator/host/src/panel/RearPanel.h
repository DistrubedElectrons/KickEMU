#pragma once

#include "config/Layout.h"
#include "panel/Painting.h"
#include "panel/Theme.h"

namespace kickemu
{
    /**
        Live status shown on the connector strips. Filled in by the session; the
        panels only read it.
    */
    struct IoStatus
    {
        std::function<bool()> powered;                       // QEMU running
        std::function<juce::uint64()> midiInCount, midiOutCount;
        std::function<juce::String()> midiInRoute, midiOutRoute;
        std::function<bool()> networkUp;
        std::function<juce::String()> networkInfo;
        std::function<float (int channel)> cvOut, cvIn;
        std::function<juce::uint32 (int channel)> cvSerial;
        std::function<int()> cvChannels;
        std::function<bool()> cvConnected;
        std::function<juce::String()> audioRoute;
        std::function<juce::String()> sdImage;               // empty when no card
        std::function<void (const juce::String& what)> onConnectorClicked;
    };

    /** Shared drawing for the rear and front strips. */
    class StripPanel : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        StripPanel (const Theme& theme, const LayoutStrip& strip, IoStatus& status);

        float getWidthMm() const { return strip.width + 2.0f * marginMm; }
        float getHeightMm() const { return strip.height + strip.footHeight + 2.0f * marginMm; }
        void setScale (float pxPerMm);

        /** Called at frame rate; repaints indicators that changed. */
        virtual void animate (double now, float dtMs) = 0;

        void paint (juce::Graphics& g) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    protected:
        virtual void drawStatic (juce::Graphics& g) const;
        virtual void drawConnector (juce::Graphics& g, const LayoutConnector& c) const;
        virtual void drawIndicators (juce::Graphics& g) = 0;
        virtual juce::String describe (const LayoutConnector& c) const = 0;

        juce::Point<float> px (juce::Point<float> mm) const { return (mm + juce::Point<float> (marginMm, marginMm)) * scale; }
        juce::Rectangle<float> px (juce::Rectangle<float> mm) const { return mm.translated (marginMm, marginMm) * scale; }
        const LayoutConnector* connectorAt (juce::Point<float> p) const;
        void drawLed (juce::Graphics& g, juce::Point<float> centre, float sizeMm, juce::Colour colour, float level) const;

        const Theme& theme;
        const LayoutStrip& strip;
        IoStatus& status;
        float scale = 3.0f, marginMm = 6.0f;
        juce::Image staticLayer;
        float staticPixelScale = 0.0f;
    };

    /** Rear connectors with MIDI activity, CV voltages, network and power state. */
    class RearPanel : public StripPanel
    {
    public:
        RearPanel (const Theme& theme, const LayoutStrip& strip, IoStatus& status);
        void animate (double now, float dtMs) override;

    private:
        void drawIndicators (juce::Graphics& g) override;
        juce::String describe (const LayoutConnector& c) const override;

        juce::uint64 lastIn = 0, lastOut = 0;
        float inGlow = 0.0f, outGlow = 0.0f;
        std::array<float, 16> cvShown {};
        bool lastPowered = false, lastNet = false;
    };

    /** Front strip: SD card slot and headphone jack. */
    class FrontPanel : public StripPanel
    {
    public:
        FrontPanel (const Theme& theme, const LayoutStrip& strip, IoStatus& status);
        void animate (double now, float dtMs) override;

    private:
        void drawIndicators (juce::Graphics& g) override;
        juce::String describe (const LayoutConnector& c) const override;
        juce::String lastSd;
        bool lastPowered = false;
    };
}
