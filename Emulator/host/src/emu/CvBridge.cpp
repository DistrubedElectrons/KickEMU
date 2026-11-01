#include "emu/CvBridge.h"

namespace kickemu
{
    CvBridge::CvBridge (const Config& c) : config (c)
    {
        numChannels = juce::jlimit (1, maxChannels, config.getInt ("host.cv.channels", 8));
        fullScale = (float) config.getDouble ("host.cv.full_scale_volts", 5.0);
        adcMax = config.getInt ("host.cv.adc_max", 4095);

        server.onData = [this] (const juce::uint8* data, size_t size)
        {
            lineBuffer += juce::String::fromUTF8 (reinterpret_cast<const char*> (data), (int) size);
            int nl;
            while ((nl = lineBuffer.indexOfChar ('\n')) >= 0)
            {
                auto line = lineBuffer.substring (0, nl).trim();
                lineBuffer = lineBuffer.substring (nl + 1);
                if (line.isNotEmpty() && ! handleLine (line) && onLog)
                    onLog ("CV: ignored line " + line);
            }
            if (lineBuffer.length() > 4096)
                lineBuffer.clear();
        };

        server.onConnectionChanged = [this] (bool connected)
        {
            lineBuffer.clear();
            if (connected)
                for (int ch = 0; ch < numChannels; ++ch)
                    if (inSet[(size_t) ch])
                        sendInput (ch);
        };
    }

    CvBridge::~CvBridge() { stop(); }

    bool CvBridge::start (juce::String& error) { return server.start (config.getSocketPath ("cv"), error); }
    void CvBridge::stop() { server.stop(); }

    bool CvBridge::handleLine (const juce::String& line)
    {
        juce::var v;
        if (juce::JSON::parse (line, v).failed() || ! v.hasProperty ("ch"))
            return false;

        const int ch = (int) v["ch"];
        if (! juce::isPositiveAndBelow (ch, numChannels))
            return false;

        if (v.hasProperty ("dac"))
        {
            const int raw = (int) v["dac"];
            outRaw[(size_t) ch] = raw;
            outVolts[(size_t) ch] = v.hasProperty ("volts") ? (float) (double) v["volts"]
                                                             : fullScale * (float) raw / (float) adcMax;
            outSerial[(size_t) ch]++;
            return true;
        }
        return false;
    }

    juce::String CvBridge::makeInputLine (int channel, float volts) const
    {
        const int adc = juce::roundToInt (juce::jlimit (0.0f, fullScale, volts) / fullScale * (float) adcMax);
        return "{\"ch\":" + juce::String (channel) + ",\"adc\":" + juce::String (adc) + "}\n";
    }

    void CvBridge::sendInput (int channel)
    {
        auto line = makeInputLine (channel, inVolts[(size_t) channel]);
        server.send (line.toRawUTF8(), line.getNumBytesAsUTF8());
    }

    void CvBridge::setInputVolts (int channel, float volts)
    {
        if (! juce::isPositiveAndBelow (channel, numChannels))
            return;
        inVolts[(size_t) channel] = juce::jlimit (0.0f, fullScale, volts);
        inSet[(size_t) channel] = true;
        sendInput (channel);
    }

    float CvBridge::getOutputVolts (int ch) const { return juce::isPositiveAndBelow (ch, maxChannels) ? outVolts[(size_t) ch].load() : 0.0f; }
    int CvBridge::getOutputRaw (int ch) const { return juce::isPositiveAndBelow (ch, maxChannels) ? outRaw[(size_t) ch].load() : 0; }
    float CvBridge::getInputVolts (int ch) const { return juce::isPositiveAndBelow (ch, maxChannels) ? inVolts[(size_t) ch].load() : 0.0f; }
    juce::uint32 CvBridge::getOutputSerial (int ch) const { return juce::isPositiveAndBelow (ch, maxChannels) ? outSerial[(size_t) ch].load() : 0u; }
}
