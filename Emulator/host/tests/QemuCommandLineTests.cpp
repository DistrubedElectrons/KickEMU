#include "emu/QemuCommandLine.h"

using namespace kickemu;

class QemuCommandLineTests : public juce::UnitTest
{
public:
    QemuCommandLineTests() : juce::UnitTest ("QEMU command line", "launcher") {}

    // A fixed host, so "auto" values resolve the same on every build machine.
    static QemuCommandLine::Host testHost()
    {
        QemuCommandLine::Host h;
        h.isMac = h.arm64 = h.hvfUsable = true;
        h.audioDriver = "coreaudio";
        return h;
    }

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

            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            auto& a = cmd.args;
            expectEquals (a[0], fakeQemu.getFullPathName());
            expectEquals (after (a, "-machine"), juce::String ("kick,emmc=emmc,sd=sd,audiodev=snd0,cv-chardev=cv,audio-tap=audiotap"));
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
            expect (after (a, "-chardev", 5).startsWith ("socket,id=audiotap,path=" + tmp.getChildFile ("run/audiotap.sock").getFullPathName()));
            expectEquals (after (a, "-qmp"), "unix:" + tmp.getChildFile ("run/qmp.sock").getFullPathName() + ",server=on,wait=off");
            expectEquals (after (a, "-display"), "vnc=unix:" + tmp.getChildFile ("run/vnc.sock").getFullPathName());
            expectEquals (after (a, "-audiodev"), juce::String ("coreaudio,id=snd0"));
            expectEquals (after (a, "-nic"), juce::String ("user,hostfwd=tcp:127.0.0.1:2222-:22"));
            expectEquals (after (a, "-kernel"), kernel.getFullPathName());
            expectEquals (after (a, "-dtb"), dtb.getFullPathName());
            expect (after (a, "-append").contains ("console=ttymxc3"));
            expect (after (a, "-drive").startsWith ("if=none,id=emmc,file="));
            expect (after (a, "-drive").endsWith (",format=raw,discard=unmap"));
            expectEquals (after (a, "-drive", 1), juce::String ("if=none,id=sd"));   // empty slot, hot-insertable
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
            auto cmd = QemuCommandLine::build (c, testHost());
            expectEquals (after (cmd.args, "-accel"), juce::String ("tcg"));
            expectEquals (after (cmd.args, "-cpu"), juce::String ("cortex-a53"));
            expect (after (cmd.args, "-machine").contains ("boot-mode=rom"));
            expect (! cmd.ok());
        }

        beginTest ("boot=uboot selects the machine boot mode and loads U-Boot");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("qemu.boot", "uboot");
            c.set ("boot.uboot_image", kernel.getFullPathName());
            c.set ("storage.emmc_image", emmc.getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            expect (after (cmd.args, "-machine").contains ("boot-mode=uboot"));
            expectEquals (after (cmd.args, "-device"),
                          "loader,file=" + kernel.getFullPathName() + ",addr=0x40200000,cpu-num=0");
            expect (! cmd.args.contains ("-kernel"));
        }

        beginTest ("kernel boot leaves the machine default, SDMA firmware is passed when present");
        {
            auto fw = tmp.getChildFile ("sdma-imx7d.bin");
            fw.replaceWithText ("f");
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.kernel_image", kernel.getFullPathName());
            c.set ("boot.dtb", dtb.getFullPathName());
            c.set ("boot.sdma_firmware", fw.getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            expect (! after (cmd.args, "-machine").contains ("boot-mode="));
            expect (after (cmd.args, "-machine").contains ("sdma-firmware=" + fw.getFullPathName()));

            c.set ("boot.sdma_firmware", tmp.getChildFile ("missing.bin").getFullPathName());
            auto missing = QemuCommandLine::build (c, testHost());
            expect (missing.ok(), missing.error);
            expect (! after (missing.args, "-machine").contains ("sdma-firmware="));
            expect (missing.warnings.joinIntoString ("\n").contains ("SDMA firmware"));
        }

        beginTest ("demo profile boots the bare-metal demo without DTB or initrd");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("demo").wasOk());
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.kernel_image", kernel.getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            expectEquals (after (cmd.args, "-kernel"), kernel.getFullPathName());
            expect (! cmd.args.contains ("-dtb"));
            expect (! cmd.args.contains ("-initrd"));
            expect (! cmd.args.contains ("-append"));
        }

        beginTest ("selftest profile boots the test initrd and runs kick-selftest");
        {
            auto initrd = tmp.getChildFile ("initramfs.cpio.gz");
            initrd.replaceWithText ("i");
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("selftest").wasOk());
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.kernel_image", kernel.getFullPathName());
            c.set ("boot.dtb", dtb.getFullPathName());
            c.set ("storage.test_initrd", initrd.getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            expectEquals (after (cmd.args, "-initrd"), initrd.getFullPathName());
            expect (after (cmd.args, "-append").contains ("kick.selftest=1"));
        }

        beginTest ("kick-app profile boots the Debian rootfs from eMMC partition 1");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("kick-app").wasOk());
            // The profile names its own eMMC image, not the test image of the default config.
            expect (c.getString ("storage.emmc_image").endsWith ("emmc-kick-app.img"));
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.kernel_image", kernel.getFullPathName());
            c.set ("boot.dtb", dtb.getFullPathName());
            c.set ("storage.emmc_image", emmc.getFullPathName());
            c.set ("storage.test_initrd", tmp.getChildFile ("initramfs.cpio.gz").getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
            expect (cmd.ok(), cmd.error);
            expectEquals (after (cmd.args, "-kernel"), kernel.getFullPathName());
            expectEquals (after (cmd.args, "-dtb"), dtb.getFullPathName());
            expect (! cmd.args.contains ("-initrd"));
            expect (after (cmd.args, "-machine").contains ("emmc=emmc"));
            expect (after (cmd.args, "-drive").contains ("file=" + emmc.getFullPathName()));
            auto append = after (cmd.args, "-append");
            expect (append.contains ("root=/dev/mmcblk2p1"));
            expect (append.contains ("console=ttymxc3,115200"));
            expect (! append.contains ("kick.selftest"));
            // The MCU stays on UART1 (ttymxc0), where KT_Kick opens it.
            expectEquals (after (cmd.args, "-serial", 0), juce::String ("chardev:mcu"));
        }

        beginTest ("virt-test profile");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("virt-test").wasOk());
            c.set ("run_dir", tmp.getFullPathName());
            c.set ("qemu.binary", fakeQemu.getFullPathName());
            c.set ("boot.firmware", kernel.getFullPathName());
            auto cmd = QemuCommandLine::build (c, testHost());
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
            auto cmd = QemuCommandLine::build (c, testHost());
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
