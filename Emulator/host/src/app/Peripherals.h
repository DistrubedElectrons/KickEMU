#pragma once

#include "config/Config.h"
#include "emu/QmpClient.h"

#include <mutex>
#include <vector>

namespace kickemu
{
    /**
        What is plugged into the KICK at run time: the SD card, WAV files on the
        audio input and output, and USB devices. Implements the QMP side of
        Emulator/docs/host-interface.md for the UI and the automation tools alike.

        State is kept as the desired configuration, so it survives a power cycle:
        the SD card goes back on the next command line, WAV files and USB devices
        are re-applied when QMP comes up again. Completion callbacks run on the QMP
        thread; onChanged may fire on any thread.
    */
    class Peripherals
    {
    public:
        using Done = std::function<void (const juce::String& error)>;

        struct UsbDevice
        {
            juce::String id, port, kind, detail;   // detail: image path or vendor:product
        };

        Peripherals (Config& config, QmpClient& qmp);

        // --- SD slot -------------------------------------------------------------------
        void insertSd (const juce::File& image, Done done);
        void ejectSd (Done done);
        juce::File getSdImage() const;
        /** While the machine is off: the card that will be in the slot at power-on. */
        void setSdOffline (const juce::File& image);

        /** Creates a sparse raw image of the given size (e.g. for a fresh card). */
        static juce::Result createImage (const juce::File& file, juce::int64 bytes);

        // --- audio files ----------------------------------------------------------------
        void setWavIn (const juce::File& file, bool loop, Done done);    // File() = off
        void setWavOut (const juce::File& file, Done done);               // File() = stop, finalise
        juce::File getWavIn() const;
        juce::File getWavOut() const;
        bool getWavInLoop() const;

        // --- USB ------------------------------------------------------------------------
        /** kind: storage (path), keyboard, mouse, tablet, host (vendor/product ids). */
        void attachUsb (const juce::String& kind, const juce::String& port, const juce::File& image,
                        int vendorId, int productId, std::function<void (const juce::String& error, const juce::String& id)> done);
        void detachUsb (const juce::String& id, Done done);
        std::vector<UsbDevice> getUsb() const;

        // --- lifecycle (Session) ----------------------------------------------------------
        /** Before QEMU starts: the card in the slot becomes the drive's file. */
        void prepareCommandLine();
        /** QMP is up after a start: re-apply WAV files and USB devices. */
        void reapply();
        /** QMP events (DEVICE_DELETED frees a USB stick's block node). */
        void handleEvent (const juce::String& event, const juce::var& data);

        std::function<void()> onChanged;
        std::function<void (const juce::String&)> onLog;

    private:
        void changed() { if (onChanged) onChanged(); }
        void log (const juce::String& s) { if (onLog) onLog (s); }
        void addUsbDevice (const UsbDevice& d, std::function<void (const juce::String&)> done);

        Config& config;
        QmpClient& qmp;
        mutable std::mutex lock;
        juce::File sdImage, wavIn, wavOut;
        bool wavInLoop = false;
        std::vector<UsbDevice> usb;
        juce::StringArray nodesToDelete;   // block nodes of USB sticks being unplugged
        int nextUsbId = 1;
    };
}
