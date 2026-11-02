#include "emu/ConsoleBridge.h"

namespace kickemu
{
    ConsoleBridge::ConsoleBridge (const Config& c, const juce::String& socketName)
        : config (c), name (socketName), server (socketName),
          model (100, 30, c.getInt ("host.ui.terminal_scrollback", 4000))
    {
        server.onData = [this] (const juce::uint8* data, size_t size)
        {
            if (terminalEnabled)
                model.feed (data, size);
            if (onRawOutput)
                onRawOutput (data, size);
        };
    }

    bool ConsoleBridge::start (juce::String& error) { return server.start (config.getSocketPath (name), error); }
    void ConsoleBridge::stop() { server.stop(); }

    void ConsoleBridge::sendText (const juce::String& text)
    {
        server.send (text.toRawUTF8(), text.getNumBytesAsUTF8());
    }
}
