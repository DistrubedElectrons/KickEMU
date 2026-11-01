#pragma once

#include "config/Config.h"
#include "emu/UartServer.h"

#include <array>

namespace kickemu
{
    /**
        CV/Gate converter link on cv.sock (JSON lines, see ARCHITECTURE.md):

          QEMU -> host on every DAC write:  {"ch":0,"dac":2048,"volts":2.500}
          host -> QEMU to drive an input:   {"ch":4,"adc":1024}
    */
    class CvBridge
    {
    public:
        static constexpr int maxChannels = 16;

        explicit CvBridge (const Config& config);
        ~CvBridge();

        bool start (juce::String& error);
        void stop();

        /** Sets the voltage the guest reads on a channel; sent immediately if connected
            and re-sent when QEMU reconnects. */
        void setInputVolts (int channel, float volts);

        float getOutputVolts (int channel) const;
        int getOutputRaw (int channel) const;
        float getInputVolts (int channel) const;
        juce::uint32 getOutputSerial (int channel) const;   // changes on every DAC write

        int getNumChannels() const { return numChannels; }
        float getFullScale() const { return fullScale; }
        bool isConnected() const { return server.isConnected(); }

        /** Parses one line from QEMU; public for tests. Returns false if not understood. */
        bool handleLine (const juce::String& line);

        /** The JSON line sent for an input value; public for tests. */
        juce::String makeInputLine (int channel, float volts) const;

        std::function<void (const juce::String&)> onLog;

    private:
        void sendInput (int channel);

        const Config& config;
        UartServer server { "cv" };
        int numChannels = 8, adcMax = 4095;
        float fullScale = 5.0f;
        juce::String lineBuffer;

        std::array<std::atomic<float>, maxChannels> outVolts {};
        std::array<std::atomic<int>, maxChannels> outRaw {};
        std::array<std::atomic<juce::uint32>, maxChannels> outSerial {};
        std::array<std::atomic<float>, maxChannels> inVolts {};
        std::array<std::atomic<bool>, maxChannels> inSet {};
    };
}
