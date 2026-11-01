#include "emu/QemuCommandLine.h"

using namespace kickemu;

class QemuCommandLineTests : public juce::UnitTest
{
public:
    QemuCommandLineTests() : juce::UnitTest ("QEMU command line", "launcher") {}

    static juce::String after (const juce::StringArray& args, const juce::String& flag, int occurrence = 0)
    {
        for (int i = 0; i < args.size() - 1; ++i)
            if (args[i] == flag && occurrence-- == 0)
                return args[i + 1];
        return {};
    }

    void runTest() override
    {
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("kickemu-cmdline-test");
        tmp.deleteRecursively();
        tmp.createDirectory();
        auto kernel = tmp.getChildFile ("Image");
        kernel.replaceWithText ("k");
        auto dtb = tmp.getChildFile ("board.dtb");
        dtb.replaceWithText ("d");
        auto emmc = tmp.getChildFile ("emmc.img");
        emmc.replaceWithText ("e");
        auto fakeQemu = tmp.getChildFile ("qemu-system-aarch64");
        fakeQemu.replaceWithText ("#!/bin/sh\n");

        juce::File repo (KICKEMU_TEST_REPO_ROOT);

        beginTest ("kick machine, kernel boot, sockets in ARCHITECTURE.md order");
        {
            Config c;
            expect (c.load (repo.getChildFile ("Emulator/config/kickemu.json")).wasOk());
            c.set ("run_dir", tmp.getChildFile ("run").getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.kernel_image", kernel.getFullPathName());
            c.set ("boot.dtb", dtb.getFullPathName());
            c.set ("storage.emmc_image", emmc.getFullPathName());
            c.set ("storage.sd_image", tmp.getChildFile ("missing-sd.img").getFullPathName());

            auto cmd = QemuCommandLine::build (c);
            expect (cmd.ok(), cmd.error);
            auto& a = cmd.args;
            expectEquals (a[0], fakeQemu.getFullPathName());
            expectEquals (after (a, "-machine"), juce::String ("kick,emmc=emmc,audiodev=snd0,cv-chardev=cv"));
            expectEquals (after (a, "-accel"), juce::String ("hvf"));
            expectEquals (after (a, "-cpu"), juce::String ("host"));
            expectEquals (after (a, "-m"), juce::String ("2048M"));
            expectEquals (after (a, "-serial", 0), juce::String ("chardev:mcu"));
            expectEquals (after (a, "-serial", 1), juce::String ("chardev:uart2"));
            expectEquals (after (a, "-serial", 2), juce::String ("chardev:midi"));
            expectEquals (after (a, "-serial", 3), juce::String ("chardev:console"));
            expect (after (a, "-chardev", 0).startsWith ("socket,id=mcu,path=" + tmp.getChildFile ("run/mcu.sock").getFullPathName()));
            expect (after (a, "-chardev", 0).endsWith (",reconnect-ms=500"));
            expect (after (a, "-chardev", 4).startsWith ("socket,id=cv,"));
            expectEquals (after (a, "-qmp"), "unix:" + tmp.getChildFile ("run/qmp.sock").getFullPathName() + ",server=on,wait=off");
            expectEquals (after (a, "-display"), "vnc=unix:" + tmp.getChildFile ("run/vnc.sock").getFullPathName());
            expectEquals (after (a, "-audiodev"), juce::String ("coreaudio,id=snd0"));
            expectEquals (after (a, "-nic"), juce::String ("user,hostfwd=tcp:127.0.0.1:2222-:22"));
            expectEquals (after (a, "-kernel"), kernel.getFullPathName());
            expectEquals (after (a, "-dtb"), dtb.getFullPathName());
            expect (after (a, "-append").contains ("console=ttymxc3"));
            expect (after (a, "-drive").startsWith ("if=none,id=emmc,file="));
            expectEquals (cmd.warnings.size(), 1); // missing SD image
        }

        beginTest ("boot=rom forces TCG and needs storage");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("qemu.boot", "rom");
            c.set ("storage.emmc_image", tmp.getChildFile ("none.img").getFullPathName());
            c.set ("storage.sd_image", tmp.getChildFile ("none2.img").getFullPathName());
            auto cmd = QemuCommandLine::build (c);
            expectEquals (after (cmd.args, "-accel"), juce::String ("tcg"));
            expectEquals (after (cmd.args, "-cpu"), juce::String ("cortex-a53"));
            expect (! cmd.ok());
        }

        beginTest ("virt-test profile");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("virt-test").wasOk());
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.firmware", kernel.getFullPathName());
            auto cmd = QemuCommandLine::build (c);
            expect (cmd.ok(), cmd.error);
            expectEquals (after (cmd.args, "-machine"), juce::String ("virt"));
            expectEquals (after (cmd.args, "-serial"), juce::String ("chardev:console"));
            expectEquals (after (cmd.args, "-serial", 1), juce::String());
            expectEquals (after (cmd.args, "-device", 1), juce::String ("virtio-multitouch-pci"));
            expectEquals (after (cmd.args, "-bios"), kernel.getFullPathName());
            expect (! cmd.args.contains ("-audiodev"));
            expect (! cmd.args.contains ("-nic"));
        }

        beginTest ("Missing kernel is an error, missing binary too");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            c.set ("qemu.binary", tmp.getChildFile ("nope").getFullPathName());
            c.set ("boot.kernel_image", tmp.getChildFile ("nokernel").getFullPathName());
            auto cmd = QemuCommandLine::build (c);
            expect (! cmd.ok());
        }

        beginTest ("Shell quoting");
        {
            QemuCommandLine cmd;
            cmd.args = { "/bin/q", "-append", "a b'c" };
            expectEquals (cmd.toShellString(), juce::String ("/bin/q -append 'a b'\\''c'"));
        }

        tmp.deleteRecursively();
    }
};

static QemuCommandLineTests qemuCommandLineTests;
