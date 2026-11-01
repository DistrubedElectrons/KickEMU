#include "emu/CvBridge.h"

using namespace kickemu;

class CvBridgeTests : public juce::UnitTest
{
public:
    CvBridgeTests() : juce::UnitTest ("CV bridge", "cv") {}

    void runTest() override
    {
        Config config;
        config.loadFromString (R"({"host":{"cv":{"channels":8,"full_scale_volts":5.0,"adc_max":4095}}})", juce::File());
        CvBridge cv (config);

        beginTest ("DAC lines from QEMU");
        {
            expect (cv.handleLine (R"({"ch":0,"dac":2048,"volts":2.500})"));
            expectWithinAbsoluteError (cv.getOutputVolts (0), 2.5f, 0.0001f);
            expectEquals (cv.getOutputRaw (0), 2048);
            expectEquals ((int) cv.getOutputSerial (0), 1);

            // Without "volts" the value is derived from the raw code.
            expect (cv.handleLine (R"({"ch":3,"dac":4095})"));
            expectWithinAbsoluteError (cv.getOutputVolts (3), 5.0f, 0.001f);

            expect (! cv.handleLine (R"({"ch":9,"dac":1})"));
            expect (! cv.handleLine ("not json"));
        }

        beginTest ("ADC lines to QEMU");
        {
            expectEquals (cv.makeInputLine (4, 1.25f), juce::String ("{\"ch\":4,\"adc\":1024}\n"));
            expectEquals (cv.makeInputLine (5, 7.0f), juce::String ("{\"ch\":5,\"adc\":4095}\n"));
            cv.setInputVolts (4, 3.3f);
            expectWithinAbsoluteError (cv.getInputVolts (4), 3.3f, 0.0001f);
        }
    }
};

static CvBridgeTests cvBridgeTests;
