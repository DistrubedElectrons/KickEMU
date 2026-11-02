#include "app/CommandLine.h"

#include <BuildInfo.h>

namespace kickemu
{
    CommandLine CommandLine::parse (const juce::StringArray& args)
    {
        CommandLine cl;
        for (int i = 0; i < args.size(); ++i)
        {
            const auto& a = args[i];
            auto value = [&]() -> juce::String
            {
                if (a.containsChar ('='))
                    return a.fromFirstOccurrenceOf ("=", false, false);
                if (i + 1 < args.size())
                    return args[++i];
                cl.error = "Missing value for " + a;
                return {};
            };

            if (a == "--help" || a == "-h") cl.help = true;
            else if (a == "--version") cl.version = true;
            else if (a == "--headless") cl.headless = true;
            else if (a == "--print-command") cl.printCommand = true;
            else if (a == "--no-start") cl.noAutostart = true;
            else if (a == "--presentation") cl.presentation = true;
            else if (a == "--mcp") { cl.mcp = true; cl.headless = true; }
            else if (a.startsWith ("--run-dir")) cl.runDir = juce::File::getCurrentWorkingDirectory().getChildFile (value()).getFullPathName();
            else if (a.startsWith ("--expect")) { cl.expect = value(); cl.headless = true; }
            else if (a.startsWith ("--timeout")) cl.timeoutSeconds = value().getIntValue();
            else if (a.startsWith ("--config")) cl.configFile = juce::File::getCurrentWorkingDirectory().getChildFile (value());
            else if (a.startsWith ("--profile")) cl.profile = value();
            else if (a == "--set") cl.overrides.add (value());
            else if (a.startsWith ("--screenshot-delay")) cl.screenshotDelayMs = value().getIntValue();
            else if (a.startsWith ("--screenshot")) cl.screenshot = juce::File::getCurrentWorkingDirectory().getChildFile (value());
            else if (a.startsWith ("--view")) cl.screenshotView = value();
            else if (a.startsWith ("--set=")) cl.overrides.add (a.fromFirstOccurrenceOf ("=", false, false));
            else if (a.startsWith ("-psn_") || a == "-NSDocumentRevisionsDebugMode" || a == "YES") continue; // macOS launch arguments
            else if (a.isNotEmpty()) cl.error = "Unknown argument: " + a;
        }
        return cl;
    }

    juce::String CommandLine::versionText()
    {
        return juce::String ("KickEmu ") + buildinfo::version + " (" + buildinfo::gitBranch + " " + buildinfo::gitCommit + ")";
    }

    juce::String CommandLine::helpText()
    {
        return versionText() + "\n"
               "Emulator host for the Behringer KICK: front panel, QEMU launcher, control-surface MCU,\n"
               "MIDI, CV/Gate and console bridges.\n"
               "\n"
               "Usage: KickEmu [options]\n"
               "\n"
               "  --config <file>      configuration file (default: Emulator/config/kickemu.json)\n"
               "  --profile <name>     apply a profile from the config (e.g. virt-test)\n"
               "  --set key.path=value override one config value; repeatable\n"
               "                       (e.g. --set qemu.accel=tcg --set qemu.smp=2)\n"
               "  --headless           no window: run the bridges and QEMU, log to stdout\n"
               "  --mcp                headless MCP server on stdio (automation tools for agents);\n"
               "                       logs go to stderr\n"
               "  --run-dir <dir>      directory for the sockets (default run_dir); use one per instance\n"
               "  --expect <regex>     headless smoke test: exit 0 once the console prints a match\n"
               "  --timeout <s>        with --expect: exit 3 if no match in time (QEMU exit before: 4)\n"
               "  --no-start           do not start QEMU automatically\n"
               "  --print-command      print the QEMU command line and exit\n"
               "  --view <top|rear|front>  view to show at start\n"
               "  --presentation       start in presentation mode (device only)\n"
               "  --screenshot <png>   save a 2x image of the window after a delay, then quit\n"
               "  --screenshot-delay <ms>  delay before --screenshot (default 4000)\n"
               "  --version            print version, branch and commit\n"
               "  -h, --help           this help\n"
               "\n"
               "Every instance serves the automation tools (MCP over JSON lines) on <run_dir>/automation.sock.\n"
               "See Emulator/docs/automation.md.\n";
    }
}
