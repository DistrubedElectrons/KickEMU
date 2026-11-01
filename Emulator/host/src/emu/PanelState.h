#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>

namespace kickemu
{
    /**
        Everything the control-surface MCU displays: button LEDs, pad colours and
        states, slider LEDs, plus what the MCU knows locally (pressed pads).

        Written by the MCU emulator on its socket thread, read by the UI at frame
        rate. Every field is an atomic so the UI never blocks the protocol and a
        burst of packets costs nothing more than a few stores. `revision` changes
        on every update, so the UI can skip frames where nothing happened.
    */
    class PanelState
    {
    public:
        static constexpr int numLeds = 80;
        static constexpr int numPads = 16;
        static constexpr int maxSliderLeds = 32;

        // Values match contracts::DeviceControlContracts::PadButtonState.
        enum PadMode : juce::uint8
        {
            padOff = 0,
            padOn = 1,
            padSampleAssigned = 2,
            padSampleNotAssigned = 3,
            padOnBlink = 4,
            padOffBlink = 5
        };

        // Values match mt::PadColorMode (engine/interface/mt_audio_interface_types.h).
        enum ColourMode : int
        {
            modeOff = 0, modeOffVelocity, modeOffFull,
            modeDimOff, modeDimVelocity, modeDimFull,
            modeOffDimOff, modeOffDimVelocity, modeOffDimFull,
            modeDisabled
        };

        enum LedMode : juce::uint8 { ledOff = 0, ledOn = 1, ledBlink = 2 };
        enum SliderMode : juce::uint8 { sliderNone = 0, sliderBar, sliderLevel, sliderIndividual };

        struct Led
        {
            std::atomic<juce::uint8> mode { ledOff };
            std::atomic<int> colourId { 0 };
            std::atomic<bool> addressed { false };   // set once the guest touched this LED
        };

        struct Pad
        {
            std::atomic<juce::uint8> mode { padOff };
            std::atomic<int> colourId { 15 };       // PadColourTrans
            std::atomic<int> blinkOffColourId { 15 };
            std::atomic<int> level { 127 };         // step-sequencer velocity (brightness)
            std::atomic<bool> blinkTwoColours { false };
            std::atomic<bool> pressed { false };    // MCU-local: a finger is on the pad
            std::atomic<int> pressVelocity { 0 };
        };

        /** What the UI should draw for a pad or LED at a given time. */
        struct Appearance
        {
            int colourId = -1;     // palette index, -1 = unlit
            float brightness = 0;  // 0..1
        };

        PanelState() { reset(); }

        /** All LEDs off, pads off, default colour mode. Used at power-on and on EN_SYS_RESET_GD. */
        void reset();

        std::array<Led, numLeds> leds;
        std::array<Pad, numPads> pads;

        std::atomic<int> colourMode { modeDimVelocity };
        std::atomic<bool> aftertouchEnabled { false };
        std::array<std::atomic<int>, 4> padSettings {};   // count, threshold, sensitivity, curve

        std::atomic<juce::uint8> sliderMode { sliderNone };
        std::atomic<int> sliderPercent { 0 };
        std::atomic<juce::uint32> sliderMask { 0 };

        std::atomic<int> bpm { 0 };
        std::atomic<bool> uartConnected { false };    // QEMU is attached to the socket
        std::atomic<bool> hostLinked { false };       // the guest answered the comm init
        std::atomic<int> swVersionMajor { 0 }, swVersionMinor { 0 };

        std::atomic<juce::uint64> packetsIn { 0 }, packetsOut { 0 }, framingErrors { 0 };
        std::atomic<juce::uint32> revision { 0 };

        void touch() { revision.fetch_add (1, std::memory_order_release); }

        /** Pad appearance after applying state, colour mode, press and blink. */
        Appearance padAppearance (int pad, double timeSeconds, double blinkPeriodSeconds) const;

        /** LED brightness (0..1) and colour id at a given time. */
        Appearance ledAppearance (int led, double timeSeconds, double blinkPeriodSeconds) const;

        /** Slider LED i (0 = bottom) brightness. */
        float sliderLed (int index, int count) const;

        /** Brightness used for "dim" pads. */
        static constexpr float dimLevel = 0.22f;
    };
}
