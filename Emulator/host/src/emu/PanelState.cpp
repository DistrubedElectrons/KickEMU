#include "emu/PanelState.h"

#include <cmath>

namespace kickemu
{
    namespace
    {
        constexpr int transparentColour = 15; // PadColourIds::PadColourTrans

        bool blinkPhase (double t, double period, bool inverted)
        {
            if (period <= 0.0)
                return ! inverted;
            bool on = std::fmod (t, period) < period * 0.5;
            return inverted ? ! on : on;
        }
    }

    void PanelState::reset()
    {
        for (auto& led : leds)
        {
            led.mode = ledOff;
            led.colourId = 0;
            led.addressed = false;
        }
        for (auto& pad : pads)
        {
            pad.mode = padOff;
            pad.colourId = transparentColour;
            pad.blinkOffColourId = transparentColour;
            pad.level = 127;
            pad.blinkTwoColours = false;
        }
        colourMode = modeDimVelocity;
        aftertouchEnabled = false;
        sliderMode = sliderNone;
        sliderPercent = 0;
        sliderMask = 0;
        bpm = 0;
        touch();
    }

    PanelState::Appearance PanelState::padAppearance (int index, double t, double period) const
    {
        Appearance a;
        if (! juce::isPositiveAndBelow (index, numPads))
            return a;

        auto& p = pads[(size_t) index];
        const auto mode = p.mode.load (std::memory_order_relaxed);
        const auto colour = p.colourId.load (std::memory_order_relaxed);
        const auto cm = colourMode.load (std::memory_order_relaxed);
        const bool pressed = p.pressed.load (std::memory_order_relaxed);
        const float velocity = (float) p.pressVelocity.load (std::memory_order_relaxed) / 127.0f;

        // Look of a pad that is not pressed (assigned / empty) and of a pressed pad,
        // following the PadColorMode naming: <empty>_<off>_<on>.
        auto offLook = [cm]() -> float
        {
            switch (cm)
            {
                case modeDimOff: case modeDimVelocity: case modeDimFull:
                case modeOffDimOff: case modeOffDimVelocity: case modeOffDimFull:
                    return dimLevel;
                case modeDisabled: return 1.0f;
                default: return 0.0f;
            }
        };
        auto emptyLook = [cm]() -> float
        {
            switch (cm)
            {
                case modeDimOff: case modeDimVelocity: case modeDimFull: return dimLevel;
                case modeDisabled: return 1.0f;
                default: return 0.0f;
            }
        };
        auto onLook = [cm, velocity]() -> float
        {
            switch (cm)
            {
                case modeOff: case modeDimOff: case modeOffDimOff: return 0.0f;
                case modeOffVelocity: case modeDimVelocity: case modeOffDimVelocity:
                    return juce::jmax (0.3f, velocity);
                default: return 1.0f;
            }
        };

        a.colourId = colour;
        switch (mode)
        {
            case padOff:
                a.brightness = 0.0f;
                break;
            case padOn:
                a.brightness = (float) p.level.load (std::memory_order_relaxed) / 127.0f;
                break;
            case padSampleAssigned:
                a.brightness = pressed ? onLook() : offLook();
                break;
            case padSampleNotAssigned:
                a.brightness = pressed ? onLook() : emptyLook();
                break;
            case padOnBlink:
            case padOffBlink:
            {
                bool on = blinkPhase (t, period, mode == padOffBlink);
                if (on)
                {
                    a.brightness = 1.0f;
                }
                else if (p.blinkTwoColours.load (std::memory_order_relaxed))
                {
                    a.colourId = p.blinkOffColourId.load (std::memory_order_relaxed);
                    a.brightness = 1.0f;
                }
                break;
            }
            default:
                break;
        }

        if (a.colourId == transparentColour || a.colourId < 0)
        {
            a.colourId = -1;
            a.brightness = 0.0f;
        }
        return a;
    }

    PanelState::Appearance PanelState::ledAppearance (int index, double t, double period) const
    {
        Appearance a;
        if (! juce::isPositiveAndBelow (index, numLeds))
            return a;

        auto& led = leds[(size_t) index];
        a.colourId = led.colourId.load (std::memory_order_relaxed);
        switch (led.mode.load (std::memory_order_relaxed))
        {
            case ledOn: a.brightness = 1.0f; break;
            case ledBlink: a.brightness = blinkPhase (t, period, false) ? 1.0f : 0.0f; break;
            default: a.brightness = 0.0f; break;
        }
        return a;
    }

    float PanelState::sliderLed (int index, int count) const
    {
        if (count <= 0 || ! juce::isPositiveAndBelow (index, count))
            return 0.0f;

        const int percent = juce::jlimit (0, 100, sliderPercent.load (std::memory_order_relaxed));
        switch (sliderMode.load (std::memory_order_relaxed))
        {
            case sliderBar:
            {
                // Fill from the bottom; any non-zero value lights at least one LED.
                int lit = (int) std::ceil (percent * count / 100.0);
                return index < lit ? 1.0f : 0.0f;
            }
            case sliderLevel:
            {
                // Centred: 50 % is the middle, the bar grows up or down from there.
                double centre = (count - 1) / 2.0;
                double pos = percent * (count - 1) / 100.0;
                double lo = juce::jmin (centre, pos), hi = juce::jmax (centre, pos);
                return (index >= std::floor (lo) && index <= std::ceil (hi)) ? 1.0f : 0.0f;
            }
            case sliderIndividual:
                return ((sliderMask.load (std::memory_order_relaxed) >> index) & 1u) != 0 ? 1.0f : 0.0f;
            default:
                return 0.0f;
        }
    }
}
