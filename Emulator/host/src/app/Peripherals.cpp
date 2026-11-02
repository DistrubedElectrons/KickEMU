#include "app/Peripherals.h"

#include "automation/ToolRegistry.h"

#include <fcntl.h>
#include <unistd.h>

namespace kickemu
{
    Peripherals::Peripherals (Config& c, QmpClient& q) : config (c), qmp (q)
    {
        sdImage = config.getPath ("storage.sd_image");
        if (! sdImage.existsAsFile())
            sdImage = juce::File();
    }

    juce::Result Peripherals::createImage (const juce::File& file, juce::int64 bytes)
    {
        // Sparse file: costs no disk until the guest writes.
        int fd = ::open (file.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0)
            return juce::Result::fail ("cannot create " + file.getFullPathName() + (errno == EEXIST ? " (exists)" : ""));
        const bool ok = ::ftruncate (fd, (off_t) bytes) == 0;
        ::close (fd);
        return ok ? juce::Result::ok() : juce::Result::fail ("cannot size " + file.getFullPathName());
    }

    // --------------------------------------------------------------------------------------

    void Peripherals::insertSd (const juce::File& image, Done done)
    {
        if (! image.existsAsFile())
        {
            done ("no such file: " + image.getFullPathName());
            return;
        }
        // A card in the slot is pulled out first, as a person would.
        auto insert = [this, image, done]
        {
            qmp.execute ("blockdev-change-medium",
                         object ({ { "device", "sd" }, { "filename", image.getFullPathName() }, { "format", "raw" } }),
                         [this, image, done] (const juce::var&, const juce::String& error)
                         {
                             if (error.isEmpty())
                             {
                                 { std::lock_guard<std::mutex> g (lock); sdImage = image; }
                                 log ("SD card inserted: " + image.getFullPathName());
                                 changed();
                             }
                             done (error);
                         });
        };
        if (getSdImage() != juce::File())
            ejectSd ([insert, done] (const juce::String& error) { if (error.isEmpty()) insert(); else done (error); });
        else
            insert();
    }

    void Peripherals::ejectSd (Done done)
    {
        qmp.execute ("eject", object ({ { "device", "sd" }, { "force", true } }),
                     [this, done] (const juce::var&, const juce::String& error)
                     {
                         if (error.isEmpty())
                         {
                             { std::lock_guard<std::mutex> g (lock); sdImage = juce::File(); }
                             log ("SD card ejected");
                             changed();
                         }
                         done (error);
                     });
    }

    void Peripherals::setSdOffline (const juce::File& image)
    {
        { std::lock_guard<std::mutex> g (lock); sdImage = image; }
        changed();
    }

    juce::File Peripherals::getSdImage() const { std::lock_guard<std::mutex> g (lock); return sdImage; }

    // --------------------------------------------------------------------------------------

    void Peripherals::setWavIn (const juce::File& file, bool loop, Done done)
    {
        qmp.execute ("qom-set", object ({ { "path", "/machine" }, { "property", "audio-wav-in-loop" }, { "value", loop } }));
        qmp.execute ("qom-set", object ({ { "path", "/machine" }, { "property", "audio-wav-in" }, { "value", file.getFullPathName() } }),
                     [this, file, loop, done] (const juce::var&, const juce::String& error)
                     {
                         if (error.isEmpty())
                         {
                             { std::lock_guard<std::mutex> g (lock); wavIn = file; wavInLoop = loop; }
                             log (file == juce::File() ? "Audio input: host" : "Audio input: " + file.getFullPathName() + (loop ? " (loop)" : ""));
                             changed();
                         }
                         done (error);
                     });
    }

    void Peripherals::setWavOut (const juce::File& file, Done done)
    {
        qmp.execute ("qom-set", object ({ { "path", "/machine" }, { "property", "audio-wav-out" }, { "value", file.getFullPathName() } }),
                     [this, file, done] (const juce::var&, const juce::String& error)
                     {
                         if (error.isEmpty())
                         {
                             juce::File previous;
                             { std::lock_guard<std::mutex> g (lock); previous = wavOut; wavOut = file; }
                             log (file == juce::File() ? "Recording stopped: " + previous.getFullPathName() : "Recording output to " + file.getFullPathName());
                             changed();
                         }
                         done (error);
                     });
    }

    juce::File Peripherals::getWavIn() const { std::lock_guard<std::mutex> g (lock); return wavIn; }
    juce::File Peripherals::getWavOut() const { std::lock_guard<std::mutex> g (lock); return wavOut; }
    bool Peripherals::getWavInLoop() const { std::lock_guard<std::mutex> g (lock); return wavInLoop; }

    // --------------------------------------------------------------------------------------

    void Peripherals::addUsbDevice (const UsbDevice& d, std::function<void (const juce::String&)> done)
    {
        auto dev = object ({ { "bus", d.port + ".0" }, { "id", d.id } });
        auto* o = dev.getDynamicObject();
        auto add = [this, dev, done]
        {
            qmp.execute ("device_add", dev, [done] (const juce::var&, const juce::String& error) { done (error); });
        };

        if (d.kind == "storage")
        {
            o->setProperty ("driver", "usb-storage");
            o->setProperty ("drive", d.id + "-blk");
            qmp.execute ("blockdev-add",
                         object ({ { "driver", "raw" }, { "node-name", d.id + "-blk" },
                                   { "file", object ({ { "driver", "file" }, { "filename", d.detail } }) } }),
                         [add, done] (const juce::var&, const juce::String& error) { if (error.isEmpty()) add(); else done (error); });
            return;
        }
        if (d.kind == "keyboard") o->setProperty ("driver", "usb-kbd");
        else if (d.kind == "mouse") o->setProperty ("driver", "usb-mouse");
        else if (d.kind == "tablet") o->setProperty ("driver", "usb-tablet");
        else if (d.kind == "host")
        {
            o->setProperty ("driver", "usb-host");
            o->setProperty ("vendorid", d.detail.upToFirstOccurrenceOf (":", false, false).getHexValue32());
            o->setProperty ("productid", d.detail.fromFirstOccurrenceOf (":", false, false).getHexValue32());
        }
        else
        {
            done ("unknown USB device kind: " + d.kind);
            return;
        }
        add();
    }

    void Peripherals::attachUsb (const juce::String& kind, const juce::String& port, const juce::File& image,
                                 int vendorId, int productId, std::function<void (const juce::String&, const juce::String&)> done)
    {
        UsbDevice d;
        d.kind = kind;
        d.port = port.isEmpty() ? juce::String ("usb-a") : port;
        if (kind == "storage")
        {
            if (! image.existsAsFile())
            {
                done ("no such file: " + image.getFullPathName(), {});
                return;
            }
            d.detail = image.getFullPathName();
        }
        else if (kind == "host")
        {
            d.detail = juce::String::toHexString (vendorId).paddedLeft ('0', 4) + ":" + juce::String::toHexString (productId).paddedLeft ('0', 4);
        }
        {
            std::lock_guard<std::mutex> g (lock);
            d.id = "usb" + juce::String (nextUsbId++);
        }
        addUsbDevice (d, [this, d, done] (const juce::String& error)
        {
            if (error.isEmpty())
            {
                { std::lock_guard<std::mutex> g (lock); usb.push_back (d); }
                log ("USB " + d.kind + " attached to " + d.port + (d.detail.isNotEmpty() ? " (" + d.detail + ")" : juce::String()));
                changed();
            }
            done (error, d.id);
        });
    }

    void Peripherals::detachUsb (const juce::String& id, Done done)
    {
        UsbDevice d;
        {
            std::lock_guard<std::mutex> g (lock);
            auto it = std::find_if (usb.begin(), usb.end(), [&] (const UsbDevice& u) { return u.id == id; });
            if (it == usb.end())
            {
                done ("no USB device " + id);
                return;
            }
            d = *it;
        }
        qmp.execute ("device_del", object ({ { "id", id } }), [this, d, done] (const juce::var&, const juce::String& error)
        {
            if (error.isEmpty())
            {
                {
                    std::lock_guard<std::mutex> g (lock);
                    // device_del finishes asynchronously; the block node goes on DEVICE_DELETED.
                    if (d.kind == "storage")
                        nodesToDelete.add (d.id);
                    usb.erase (std::remove_if (usb.begin(), usb.end(), [&] (const UsbDevice& u) { return u.id == d.id; }), usb.end()); }
                log ("USB " + d.kind + " detached");
                changed();
            }
            done (error);
        });
    }

    std::vector<Peripherals::UsbDevice> Peripherals::getUsb() const { std::lock_guard<std::mutex> g (lock); return usb; }

    // --------------------------------------------------------------------------------------

    void Peripherals::handleEvent (const juce::String& event, const juce::var& data)
    {
        if (event != "DEVICE_DELETED")
            return;
        const auto id = data["device"].toString();
        {
            std::lock_guard<std::mutex> g (lock);
            if (! nodesToDelete.contains (id))
                return;
            nodesToDelete.removeString (id);
        }
        qmp.execute ("blockdev-del", object ({ { "node-name", id + "-blk" } }));
    }

    void Peripherals::prepareCommandLine()
    {
        auto card = getSdImage();
        config.set ("storage.sd_image", card.existsAsFile() ? card.getFullPathName() : juce::String());
    }

    void Peripherals::reapply()
    {
        juce::File in, out;
        bool loop;
        std::vector<UsbDevice> devices;
        {
            std::lock_guard<std::mutex> g (lock);
            in = wavIn; out = wavOut; loop = wavInLoop; devices = usb;
        }
        auto report = [this] (const juce::String& what) { return [this, what] (const juce::String& e) { if (e.isNotEmpty()) log (what + ": " + e); }; };
        if (in != juce::File())
            setWavIn (in, loop, report ("Audio input file"));
        if (out != juce::File())
        {
            // A new recording per power cycle: the previous file was finalised at exit.
            auto next = out.getNonexistentSibling();
            setWavOut (next, report ("Recording"));
        }
        for (auto& d : devices)
            addUsbDevice (d, report ("USB " + d.kind));
    }
}
