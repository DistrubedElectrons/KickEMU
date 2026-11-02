#include "emu/QemuCommandLine.h"

#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace kickemu
{
    namespace
    {
        // Looks a bare program name up in PATH, like a shell would.
        juce::File findInPath (const juce::String& name)
        {
            auto path = juce::SystemStats::getEnvironmentVariable ("PATH", "/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin");
            for (auto& dir : juce::StringArray::fromTokens (path, ":", {}))
            {
                auto candidate = juce::File (dir).getChildFile (name);
                if (candidate.existsAsFile())
                    return candidate;
            }
            return {};
        }

        juce::String quote (const juce::String& s)
        {
            if (s.isNotEmpty() && s.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_=.,:/+@%"))
                return s;
            return "'" + s.replace ("'", "'\\''") + "'";
        }
    }

    juce::String QemuCommandLine::toShellString() const
    {
        juce::StringArray quoted;
        for (auto& a : args)
            quoted.add (quote (a));
        return quoted.joinIntoString (" ");
    }

    QemuCommandLine::Host QemuCommandLine::Host::detect()
    {
        Host h;
       #if JUCE_MAC
        h.macOS = true;
       #elif JUCE_LINUX
        h.linux = true;
       #endif
        utsname u {};
        if (uname (&u) == 0)
            h.arm64 = juce::String (u.machine) == "arm64" || juce::String (u.machine) == "aarch64";
        if (h.linux)
        {
            int fd = ::open ("/dev/kvm", O_RDWR | O_CLOEXEC);
            h.kvmUsable = fd >= 0;
            if (fd >= 0)
                ::close (fd);
        }

        // audiodev=auto: the native API on macOS; on Linux the first sound server
        // that is actually running, then ALSA if there is a sound card, else none.
        if (h.macOS)
            h.audioDriver = "coreaudio";
        else
        {
            auto runtime = juce::File (juce::SystemStats::getEnvironmentVariable ("XDG_RUNTIME_DIR", {}));
            if (runtime != juce::File() && runtime.getChildFile ("pipewire-0").exists())
                h.audioDriver = "pipewire";
            else if (runtime != juce::File() && runtime.getChildFile ("pulse/native").exists())
                h.audioDriver = "pa";
            else if (juce::File ("/dev/snd").isDirectory())
                h.audioDriver = "alsa";
            else
                h.audioDriver = "none";
        }
        return h;
    }

    juce::String QemuCommandLine::resolveAccel (const juce::String& configured, const Host& host)
    {
        if (configured != "auto")
            return configured;
        if (host.macOS && host.arm64)
            return "hvf";
        if (host.linux && host.arm64 && host.kvmUsable)
            return "kvm";
        return "tcg";
    }

    QemuCommandLine QemuCommandLine::build (const Config& config)
    {
        return build (config, Host::detect());
    }

    QemuCommandLine QemuCommandLine::build (const Config& config, const Host& host)
    {
        QemuCommandLine cmd;
        auto& args = cmd.args;

        // --- binary --------------------------------------------------------------------
        auto binaryName = Config::expandEnvironment (config.getString ("qemu.binary"));
        juce::File binary = binaryName.containsChar ('/') ? config.resolvePath (binaryName) : findInPath (binaryName);
        if (! binary.existsAsFile())
        {
            cmd.error = "QEMU binary not found: " + (binaryName.containsChar ('/') ? config.resolvePath (binaryName).getFullPathName() : binaryName)
                        + " (set qemu.binary or build it with Emulator/qemu/build.sh)";
            args.add (binaryName);
        }
        else
        {
            args.add (binary.getFullPathName());
        }

        const auto boot = config.getString ("qemu.boot", "kernel");
        auto accel = resolveAccel (config.getString ("qemu.accel", "auto"), host);
        if (boot == "rom" && accel != "tcg")
        {
            cmd.warnings.add ("boot=rom runs the BootROM, SPL and ATF at EL3, which needs TCG; using tcg instead of " + accel);
            accel = "tcg";
        }

        // --- storage (decides which machine properties can be set) ---------------------
        juce::StringArray machineProps;
        juce::StringArray driveArgs;
        bool haveBootableImage = false;
        // Guest discards and eMMC erases punch holes into the sparse images.
        const auto discard = config.getString ("storage.discard", "unmap");
        auto addDrive = [&] (const juce::String& id, const juce::String& imageKey, const juce::String& propKey, bool removable)
        {
            auto prop = config.getString (propKey);
            if (prop.isEmpty())
                return;
            auto image = config.getPath (imageKey);
            juce::String spec = "if=none,id=" + id;
            if (image.existsAsFile())
            {
                spec << ",file=" << image.getFullPathName() << ",format=raw";
                haveBootableImage = true;
                if (discard.isNotEmpty())
                    spec << ",discard=" << discard;
            }
            else if (removable)
            {
                // Empty slot: the drive exists so a card can be inserted at run time.
                if (image != juce::File())
                    cmd.warnings.add (id + " image not found, starting with an empty slot: " + image.getFullPathName());
            }
            else
            {
                cmd.warnings.add (id + " image not found, starting without it: " + image.getFullPathName());
                return;
            }
            driveArgs.add ("-drive");
            driveArgs.add (spec);
            machineProps.add (prop + "=" + id);
        };
        addDrive ("emmc", "storage.emmc_image", "qemu.emmc_property", false);
        addDrive ("sd", "storage.sd_image", "qemu.sd_property", true);

        auto audiodev = config.getString ("qemu.audiodev");
        if (audiodev == "auto")
            audiodev = host.audioDriver.isNotEmpty() ? host.audioDriver : juce::String ("none");
        const auto audiodevId = config.getString ("qemu.audiodev_id", "snd0");
        if (audiodev.isNotEmpty() && config.getString ("qemu.audio_property").isNotEmpty())
            machineProps.add (config.getString ("qemu.audio_property") + "=" + audiodevId);

        const auto cvProperty = config.getString ("qemu.cv_property");
        if (cvProperty.isNotEmpty())
            machineProps.add (cvProperty + "=cv");

        // The kick machine defaults to direct kernel boot; U-Boot and BootROM boots are
        // machine boot modes, the image itself is placed below.
        const auto bootProperty = config.getString ("qemu.boot_property");
        if (bootProperty.isNotEmpty() && (boot == "uboot" || boot == "rom"))
            machineProps.add (bootProperty + "=" + boot);

        // SDMA RAM firmware of the guest rootfs: the machine reads its script table
        const auto fwProperty = config.getString ("qemu.sdma_firmware_property");
        if (auto fw = config.getPath ("boot.sdma_firmware"); fwProperty.isNotEmpty() && fw != juce::File())
        {
            if (fw.existsAsFile())
                machineProps.add (fwProperty + "=" + fw.getFullPathName());
            else
                cmd.warnings.add ("SDMA firmware not found, using script inference: " + fw.getFullPathName());
        }

        if (auto* extra = config.get ("qemu.machine_properties").getDynamicObject())
            for (auto& p : extra->getProperties())
                machineProps.add (p.name.toString() + "=" + p.value.toString());

        // --- machine, cpu, memory ---------------------------------------------------------
        auto machine = config.getString ("qemu.machine", "kick");
        args.add ("-machine");
        args.add (machineProps.isEmpty() ? machine : machine + "," + machineProps.joinIntoString (","));

        args.add ("-accel");
        args.add (accel);

        auto cpu = accel == "tcg" ? config.getString ("qemu.tcg_cpu") : config.getString ("qemu.cpu");
        if (cpu.isNotEmpty())
        {
            args.add ("-cpu");
            args.add (cpu);
        }

        args.add ("-smp");
        args.add (juce::String (config.getInt ("qemu.smp", 1)));
        args.add ("-m");
        args.add (juce::String (config.getInt ("qemu.memory_mb", 1024)) + "M");

        // Nothing on stdio: the console and monitor go through sockets.
        args.add ("-monitor");
        args.add ("none");
        args.add ("-parallel");
        args.add ("none");

        // --- UARTs and the CV chardev (QEMU connects to our listening sockets) -----------
        const auto reconnect = juce::String (config.getInt ("host.chardev_reconnect_ms", 500));
        auto chardev = [&] (const juce::String& id, const juce::String& socketName)
        {
            args.add ("-chardev");
            args.add ("socket,id=" + id + ",path=" + config.getSocketPath (socketName).getFullPathName() + ",reconnect-ms=" + reconnect);
        };

        auto serialOrder = config.getStringArray ("qemu.serial_order");
        if (serialOrder.isEmpty())
            serialOrder = { "mcu", "uart2", "midi", "console" };
        for (auto& name : serialOrder)
        {
            chardev (name, name);
            args.add ("-serial");
            args.add ("chardev:" + name);
        }

        if (cvProperty.isNotEmpty())
            chardev ("cv", "cv");

        // --- QMP and display (QEMU is the server) -------------------------------------------
        args.add ("-qmp");
        args.add ("unix:" + config.getSocketPath ("qmp").getFullPathName() + ",server=on,wait=off");

        args.add ("-display");
        if (config.getString ("qemu.display", "vnc") == "vnc")
            args.add ("vnc=unix:" + config.getSocketPath ("vnc").getFullPathName());
        else
            args.add ("none");

        // --- audio, network, extra devices -----------------------------------------------------
        if (audiodev.isNotEmpty())
        {
            args.add ("-audiodev");
            args.add (audiodev + ",id=" + audiodevId);
        }

        if (config.getBool ("qemu.network", false))
        {
            juce::String nic = "user";
            if (auto model = config.getString ("qemu.nic_model"); model.isNotEmpty())
                nic << ",model=" << model;
            if (auto port = config.getInt ("qemu.ssh_host_port", 0); port > 0)
                nic << ",hostfwd=tcp:127.0.0.1:" << port << "-:22";
            args.add ("-nic");
            args.add (nic);
        }

        for (auto& device : config.getStringArray ("qemu.devices"))
        {
            args.add ("-device");
            args.add (device);
        }

        args.addArray (driveArgs);

        // --- boot ------------------------------------------------------------------------------------
        auto requireFile = [&cmd] (const juce::File& f, const juce::String& what) -> bool
        {
            if (f.existsAsFile())
                return true;
            if (cmd.error.isEmpty())
                cmd.error = what + " not found: " + (f == juce::File() ? juce::String ("(not configured)") : f.getFullPathName());
            return false;
        };

        if (boot == "kernel")
        {
            auto kernel = config.getPath ("boot.kernel_image");
            if (requireFile (kernel, "Kernel image (boot.kernel_image)"))
            {
                args.add ("-kernel");
                args.add (kernel.getFullPathName());
            }
            if (auto dtb = config.getPath ("boot.dtb"); dtb != juce::File())
            {
                if (requireFile (dtb, "Device tree (boot.dtb)"))
                {
                    args.add ("-dtb");
                    args.add (dtb.getFullPathName());
                }
            }
            if (config.getBool ("storage.use_test_initrd", false))
            {
                auto initrd = config.getPath ("storage.test_initrd");
                if (requireFile (initrd, "Test initrd (storage.test_initrd)"))
                {
                    args.add ("-initrd");
                    args.add (initrd.getFullPathName());
                }
            }
            if (auto cmdline = config.getString ("boot.cmdline"); cmdline.isNotEmpty())
            {
                args.add ("-append");
                args.add (cmdline);
            }
        }
        else if (boot == "uboot")
        {
            auto uboot = config.getPath ("boot.uboot_image");
            if (requireFile (uboot, "U-Boot image (boot.uboot_image)"))
            {
                args.add ("-device");
                args.add ("loader,file=" + uboot.getFullPathName() + ",addr=" + config.getString ("boot.uboot_load_addr", "0x40200000") + ",cpu-num=0");
            }
        }
        else if (boot == "rom")
        {
            if (! haveBootableImage && cmd.error.isEmpty())
                cmd.error = "boot=rom needs an eMMC or SD image holding imx-boot (storage.emmc_image / storage.sd_image)";
        }
        else if (boot == "firmware")
        {
            auto fw = config.getPath ("boot.firmware");
            if (requireFile (fw, "Firmware (boot.firmware)"))
            {
                args.add ("-bios");
                args.add (fw.getFullPathName());
            }
        }
        else if (boot != "none")
        {
            if (cmd.error.isEmpty())
                cmd.error = "Unknown boot mode '" + boot + "' (use kernel, uboot, rom, firmware or none)";
        }

        for (auto& extra : config.getStringArray ("qemu.extra_args"))
            args.add (Config::expandEnvironment (extra));

        return cmd;
    }
}
