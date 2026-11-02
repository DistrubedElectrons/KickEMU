// Automation core: console capture, the MCP protocol handler, accelerator choice.

#include "automation/ConsoleCapture.h"
#include "automation/McpServer.h"
#include "emu/QemuCommandLine.h"

using namespace kickemu;

class AutomationTests : public juce::UnitTest
{
public:
    AutomationTests() : juce::UnitTest ("Automation", "Automation") {}

    static void feed (ConsoleCapture& c, const char* s) { c.append ((const juce::uint8*) s, std::strlen (s)); }

    void runTest() override
    {
        beginTest ("Console capture strips escapes and matches lines split across reads");
        {
            ConsoleCapture c;
            feed (c, "\x1b[1;32mkick\x1b[0m login");
            feed (c, ": \r\n");
            expectEquals (c.read (0).text, juce::String ("kick login: \n"));
            juce::String error;
            const auto mark = c.end();
            feed (c, "Welcome to Alpi");
            feed (c, "ne\nkick:~# ");
            auto m = c.waitFor ("Welcome to (\\w+)", mark, 0, error);
            expect (m.found);
            expectEquals (m.context, juce::String ("Welcome to Alpine"));
            expect (! c.waitFor ("login:", m.matchEnd, 0, error).found, "output before 'since' is not new");
            expect (c.waitFor ("[#$] $", mark, 0, error).found);
            expect (! c.waitFor ("(", 0, 0, error).found && error.isNotEmpty());
        }

        beginTest ("Console capture waits for output from another thread");
        {
            ConsoleCapture c;
            juce::Thread::launch ([&c] { juce::Thread::sleep (50); feed (c, "boot done\n"); });
            juce::String error;
            expect (c.waitFor ("boot done", 0, 2000, error).found);
        }

        beginTest ("MCP: initialize, tools/list, tools/call, errors, notifications");
        {
            ToolRegistry reg;
            reg.add ({ "echo", "echo", Schema().string ("text", "", true),
                       [] (const juce::var& a) { return ToolResult::ok (object ({ { "text", a["text"] } })); } });
            McpServer mcp (reg, "kickemu", "1.0");

            auto call = [&mcp] (const juce::String& line) { return juce::JSON::parse (mcp.handle (line).value_or ("null")); };
            auto init = call (R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
            expectEquals (init["result"]["protocolVersion"].toString(), juce::String ("2025-03-26"));
            expect (! mcp.handle (R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());
            expectEquals (call (R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})")["result"]["tools"][0]["name"].toString(), juce::String ("echo"));

            auto ok = call (R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})");
            expect (! (bool) ok["result"]["isError"]);
            expect (ok["result"]["content"][0]["text"].toString().contains ("hi"));
            auto missing = call (R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
            expect ((bool) missing["result"]["isError"]);
            expectEquals ((int) call (R"({"jsonrpc":"2.0","id":5,"method":"nope"})")["error"]["code"], -32601);
            expectEquals ((int) call ("{oops")["error"]["code"], -32700);
        }

        beginTest ("accel=auto picks the hypervisor the host has");
        {
            QemuCommandLine::Host mac { true, false, true, false, true };
            QemuCommandLine::Host macVm { true, false, true, false, false };
            QemuCommandLine::Host linuxKvm { false, true, true, true };
            QemuCommandLine::Host linuxNoKvm { false, true, true, false };
            QemuCommandLine::Host linuxX86 { false, true, false, true };
            expectEquals (QemuCommandLine::resolveAccel ("auto", mac), juce::String ("hvf"));
            expectEquals (QemuCommandLine::resolveAccel ("auto", macVm), juce::String ("tcg"));
            expectEquals (QemuCommandLine::resolveAccel ("auto", linuxKvm), juce::String ("kvm"));
            expectEquals (QemuCommandLine::resolveAccel ("auto", linuxNoKvm), juce::String ("tcg"));
            expectEquals (QemuCommandLine::resolveAccel ("auto", linuxX86), juce::String ("tcg"));
            expectEquals (QemuCommandLine::resolveAccel ("tcg", mac), juce::String ("tcg"));
        }
    }
};

static AutomationTests automationTests;
