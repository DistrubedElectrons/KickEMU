#include "emu/MidiBridge.h"

namespace kickemu
{
    MidiBridge::MidiBridge (const Config& c) : config (c)
    {
        outName = config.getString ("midi.virtual_out_name", "KICK MIDI Out");
        inName = config.getString ("midi.virtual_in_name", "KICK MIDI In");

        parser.onMessage = [this] (const juce::uint8* data, size_t size) { toHost (data, size); };
        server.onData = [this] (const juce::uint8* data, size_t size) { parser.feed (data, size); };
        server.onConnectionChanged = [this] (bool) { parser.reset(); };
    }

    MidiBridge::~MidiBridge() { stop(); }

    bool MidiBridge::start (juce::String& error)
    {
        if (! server.start (config.getSocketPath ("midi"), error))
            return false;

        // Virtual ports: other apps see "KICK MIDI Out" as a source and
        // "KICK MIDI In" as a destination, like the DIN jacks.
        {
            std::lock_guard<std::mutex> lock (outputLock);
            virtualOut = juce::MidiOutput::createNewDevice (outName);
        }
        virtualIn = juce::MidiInput::createNewDevice (inName, this);
        if (virtualIn != nullptr)
            virtualIn->start();

        if (virtualOut == nullptr || virtualIn == nullptr)
        {
            if (onLog)
                onLog ("MIDI: could not create the virtual ports (no CoreMIDI / ALSA sequencer?)");
        }
        else if (onLog)
        {
            onLog ("MIDI: virtual ports \"" + outName + "\" and \"" + inName + "\" ready");
        }
        return true;
    }

    void MidiBridge::stop()
    {
        if (virtualIn != nullptr)
            virtualIn->stop();
        if (deviceIn != nullptr)
            deviceIn->stop();
        virtualIn.reset();
        deviceIn.reset();
        {
            std::lock_guard<std::mutex> lock (outputLock);
            virtualOut.reset();
            deviceOut.reset();
        }
        server.stop();
    }

    void MidiBridge::setOutputDevice (const juce::String& identifier)
    {
        std::lock_guard<std::mutex> lock (outputLock);
        outputId = identifier;
        deviceOut.reset();
        if (identifier.isNotEmpty())
        {
            deviceOut = juce::MidiOutput::openDevice (identifier);
            if (deviceOut == nullptr && onLog)
                onLog ("MIDI: could not open output " + identifier);
        }
    }

    void MidiBridge::setInputDevice (const juce::String& identifier)
    {
        if (deviceIn != nullptr)
            deviceIn->stop();
        deviceIn.reset();
        inputId = identifier;
        if (identifier.isNotEmpty())
        {
            deviceIn = juce::MidiInput::openDevice (identifier, this);
            if (deviceIn != nullptr)
                deviceIn->start();
            else if (onLog)
                onLog ("MIDI: could not open input " + identifier);
        }
    }

    void MidiBridge::toHost (const juce::uint8* data, size_t size)
    {
        juce::MidiMessage message (data, (int) size, juce::Time::getMillisecondCounterHiRes() * 0.001);
        messagesFromGuest++;
        if (onGuestMessage)
            onGuestMessage (data, size);
        if (onMonitor)
            onMonitor (false, data, size);
        std::lock_guard<std::mutex> lock (outputLock);
        if (virtualOut != nullptr)
            virtualOut->sendMessageNow (message);
        if (deviceOut != nullptr)
            deviceOut->sendMessageNow (message);
    }

    bool MidiBridge::sendToGuest (const juce::uint8* data, size_t size)
    {
        if (! server.send (data, size))
            return false;
        messagesToGuest++;
        if (onMonitor)
            onMonitor (true, data, size);
        return true;
    }

    void MidiBridge::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
    {
        messagesToGuest++;
        if (server.send (message.getRawData(), (size_t) message.getRawDataSize()) && onMonitor)
            onMonitor (true, message.getRawData(), (size_t) message.getRawDataSize());
    }
}
