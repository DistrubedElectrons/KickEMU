#include "config/Config.h"

using namespace kickemu;

class ConfigTests : public juce::UnitTest
{
public:
    ConfigTests() : juce::UnitTest ("Config", "config") {}

    void runTest() override
    {
        const juce::File repo (KICKEMU_TEST_REPO_ROOT);

        beginTest ("Repository config loads");
        {
            Config c;
            auto r = c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (c.getString ("qemu.machine"), juce::String ("kick"));
            expect (c.getRepoRoot() == repo);
            expect (c.getPath ("qemu.binary") == repo.getChildFile ("Emulator/out/qemu/bin/qemu-system-aarch64"));
            expect (c.getProfileNames().contains ("virt-test"));
            expect (c.getSocketPath ("mcu").getFileName() == "mcu.sock");
            expect (! c.getRunDir().getFullPathName().contains ("${"));
        }

        beginTest ("Profile merge keeps untouched keys");
        {
            Config c;
            c.load (repo.getChildFile ("Emulator/config/kickemu.json"));
            expect (c.applyProfile ("virt-test").wasOk());
            expectEquals (c.getString ("qemu.machine"), juce::String ("virt"));
            expectEquals (c.getString ("qemu.tcg_cpu"), juce::String ("cortex-a53"));
            expect (c.getStringArray ("qemu.serial_order") == juce::StringArray ("console"));
            expect (c.applyProfile ("nope").failed());
        }

        beginTest ("Overrides parse JSON values");
        {
            Config c;
            c.loadFromString (R"({"qemu":{"smp":4}})", repo);
            expect (c.applyOverride ("qemu.smp=2").wasOk());
            expectEquals (c.getInt ("qemu.smp"), 2);
            expect (c.applyOverride ("qemu.accel=tcg").wasOk());
            expectEquals (c.getString ("qemu.accel"), juce::String ("tcg"));
            expect (c.applyOverride ("new.deep.key=[1,2]").wasOk());
            expectEquals (c.getIntArray ("new.deep.key").size(), 2);
            expect (c.applyOverride ("broken").failed());
        }

        beginTest ("Environment expansion");
        {
            auto home = juce::SystemStats::getEnvironmentVariable ("HOME", {});
            expectEquals (Config::expandEnvironment ("${HOME}/x"), home + "/x");
            expectEquals (Config::expandEnvironment ("${KICKEMU_SURELY_UNSET_VAR}x"), juce::String ("x"));
            expect (Config::expandEnvironment ("~/a").startsWith ("/"));
        }

        beginTest ("Hex strings as integers");
        {
            Config c;
            c.loadFromString (R"({"a":"0x40200000","b":"17"})", repo);
            expectEquals (c.getInt ("a"), 0x40200000);
            expectEquals (c.getInt ("b"), 17);
        }
    }
};

static ConfigTests configTests;
