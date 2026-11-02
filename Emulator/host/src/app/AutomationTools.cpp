#include "app/AutomationTools.h"

#include <contracts/dcl/dcl_contracts.h>
#include <juce_events/juce_events.h>
#include <thread>

namespace kickemu
{
    using DC = contracts::DeviceControlContracts;

    namespace
    {
        void sleepMs (int ms)
        {
            if (ms > 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (ms));
        }

        juce::String stateName (Session::QemuState s)
        {
            switch (s)
            {
                case Session::QemuState::running: return "running";
                case Session::QemuState::stopping: return "stopping";
                case Session::QemuState::stopped: break;
            }
            return "stopped";
        }

        /** Bytes from an array of numbers or a hex string ("90 3c 64" / "903c64"). */
        bool parseBytes (const juce::var& v, std::vector<juce::uint8>& out, juce::String& error)
        {
            if (auto* a = v.getArray())
            {
                for (auto& b : *a)
                {
                    int x = (int) b;
                    if (x < 0 || x > 255)
                    {
                        error = "byte out of range: " + b.toString();
                        return false;
                    }
                    out.push_back ((juce::uint8) x);
                }
                return true;
            }
            auto hex = v.toString().removeCharacters (" ,:");
            if (hex.length() % 2 != 0 || ! hex.containsOnly ("0123456789abcdefABCDEF"))
            {
                error = "bytes must be an array of numbers or a hex string";
                return false;
            }
            for (int i = 0; i < hex.length(); i += 2)
                out.push_back ((juce::uint8) hex.substring (i, i + 2).getHexValue32());
            return true;
        }

        juce::String toHex (const std::vector<juce::uint8>& bytes)
        {
            juce::StringArray parts;
            for (auto b : bytes)
                parts.add (juce::String::toHexString ((int) b).paddedLeft ('0', 2));
            return parts.joinIntoString (" ");
        }
    }

    AutomationTools::AutomationTools (Session& s, const Layout& l) : session (s), layout (l)
    {
        addMachineTools();
        addConsoleTools();
        addScreenTools();
        addPanelTools();
        addIoTools();
        addMediaTools();
    }

    juce::String AutomationTools::instructions()
    {
        return "KickEmu drives an emulated Behringer KICK (i.MX 8M Mini) running in QEMU. "
               "Typical flow: status -> machine_start (if stopped) -> console_wait for a login or "
               "shell prompt -> console_run commands -> screenshot to see the 1024x600 panel -> "
               "press/turn/pad/touch to operate the device. Offsets returned by console_* are "
               "absolute; pass the returned 'next' as 'since' to read only new output.";
    }

    // --------------------------------------------------------------------------------------
    // helpers

    bool AutomationTools::onMessageThread (std::function<void()> fn, int timeoutMs)
    {
        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        {
            fn();
            return true;
        }
        auto done = std::make_shared<juce::WaitableEvent>();
        juce::MessageManager::callAsync ([action = std::move (fn), done] { action(); done->signal(); });
        return done->wait (timeoutMs);
    }

    ToolResult AutomationTools::qmp (const juce::String& command, const juce::var& arguments, int timeoutMs)
    {
        if (session.getQemuState() != Session::QemuState::running)
            return ToolResult::fail ("QEMU is not running");

        struct Reply
        {
            juce::WaitableEvent done;
            juce::var result;
            juce::String error;
        };
        auto reply = std::make_shared<Reply>();
        session.qmp.execute (command, arguments, [reply] (const juce::var& result, const juce::String& error)
        {
            reply->result = result;
            reply->error = error;
            reply->done.signal();
        });
        if (! reply->done.wait (timeoutMs))
            return ToolResult::fail ("QMP " + command + ": no reply within " + juce::String (timeoutMs) + " ms");
        if (reply->error.isNotEmpty())
            return ToolResult::fail ("QMP " + command + ": " + reply->error);
        return ToolResult::ok (reply->result.isVoid() ? juce::var (new juce::DynamicObject()) : reply->result);
    }

    const LayoutControl* AutomationTools::control (const juce::String& id) const
    {
        if (auto* c = layout.find (id))
            return c;
        // Accept the printed label too ("TAP TEMPO" for tap_tempo).
        for (auto& c : layout.controls)
            if (c.label.replace ("\n", " ").equalsIgnoreCase (id) || c.id.equalsIgnoreCase (id.replace (" ", "_")))
                return &c;
        return nullptr;
    }

    // --------------------------------------------------------------------------------------
    // machine

    void AutomationTools::addMachineTools()
    {
        tools.add ({ "status",
                     "Machine state, uptime, link state of the MCU/console/MIDI/CV/display, profile and QEMU command line.",
                     Schema(),
                     [this] (const juce::var&)
                     {
                         auto& cfg = session.config;
                         auto& fb = session.framebuffer;
                         int w = 0, h = 0;
                         {
                             std::lock_guard<std::mutex> guard (fb.lock);
                             w = fb.image.getWidth();
                             h = fb.image.getHeight();
                         }
                         const auto link = session.mcu.protocol().getLinkState();
                         return ToolResult::ok (object ({
                             { "qemu", stateName (session.getQemuState()) },
                             { "uptime_ms", session.getUptimeMs() },
                             { "profile", cfg.getActiveProfile() },
                             { "accel", QemuCommandLine::resolveAccel (cfg.getString ("qemu.accel", "auto"), QemuCommandLine::Host::detect()) },
                             { "run_dir", cfg.getRunDir().getFullPathName() },
                             { "mcu", link == McuProtocol::LinkState::linked ? "linked"
                                    : link == McuProtocol::LinkState::waitingForHost ? "waiting for the app" : "disconnected" },
                             { "console_connected", session.console.isConnected() },
                             { "console_offset", (juce::int64) session.consoleText.end() },
                             { "midi_connected", session.midi.isGuestConnected() },
                             { "cv_connected", session.cv.isConnected() },
                             { "qmp_ready", session.qmp.isReady() },
                             { "display", object ({ { "connected", fb.connected.load() }, { "width", w }, { "height", h },
                                                    { "frame", (juce::int64) fb.serial.load() } }) },
                             { "command_line", session.getLastCommandLine() } }));
                     } });

        auto power = [this] (const char* name, const char* description, std::function<void()> action, Session::QemuState expect)
        {
            tools.add ({ name, description, Schema(),
                         [this, action, expect] (const juce::var&)
                         {
                             if (! onMessageThread (action))
                                 return ToolResult::fail ("the host did not respond");
                             // stop/restart finish asynchronously: wait for the state.
                             for (int i = 0; i < 100 && session.getQemuState() != expect; ++i)
                                 sleepMs (50);
                             return ToolResult::ok (object ({ { "qemu", stateName (session.getQemuState()) },
                                                              { "command_line", session.getLastCommandLine() } }));
                         } });
        };
        power ("machine_start", "Power the emulated KICK on (start QEMU). No effect if it is running.",
               [this] { session.startQemu(); }, Session::QemuState::running);
        power ("machine_stop", "Power off (QMP quit, killed after host.qemu_quit_timeout_ms).",
               [this] { session.stopQemu(); }, Session::QemuState::stopped);
        power ("machine_restart", "Power cycle the machine (stop, then start).",
               [this] { session.restartQemu(); }, Session::QemuState::running);

        tools.add ({ "power_button",
                     "Press the power key (SNVS ON/OFF): a short press asks Linux to power off cleanly.",
                     Schema(),
                     [this] (const juce::var&) { return qmp ("system_powerdown", {}); } });

        tools.add ({ "qmp",
                     "Run a raw QMP command (e.g. query-status, query-block, savevm via human-monitor-command).",
                     Schema().string ("command", "QMP command name", true).string ("arguments", "arguments as a JSON object string"),
                     [this] (const juce::var& a)
                     {
                         juce::var arguments;
                         if (auto text = args::str (a, "arguments"); text.isNotEmpty())
                             if (juce::JSON::parse (text, arguments).failed())
                                 return ToolResult::fail ("arguments is not valid JSON");
                         return qmp (args::str (a, "command"), arguments);
                     } });

        tools.add ({ "hmp",
                     "Run a QEMU human monitor command (e.g. 'info usb', 'info qtree', 'savevm s1', 'loadvm s1').",
                     Schema().string ("command", "monitor command line", true),
                     [this] (const juce::var& a)
                     {
                         auto r = qmp ("human-monitor-command", object ({ { "command-line", args::str (a, "command") } }), 60000);
                         if (! r.failed())
                             r.data = object ({ { "output", r.data.toString() } });
                         return r;
                     } });

        tools.add ({ "log_read",
                     "Host log (qemu stderr, mcu, midi, cv, qmp, vnc, host) since an index.",
                     Schema().integer ("since", "first entry index (from a previous 'next')").string ("source", "only this source").integer ("max", "max entries (default 200)"),
                     [this] (const juce::var& a)
                     {
                         juce::uint64 cursor = (juce::uint64) juce::jmax (0, args::integer (a, "since"));
                         auto entries = session.log.since (cursor);
                         const auto source = args::str (a, "source");
                         const auto max = (size_t) juce::jmax (1, args::integer (a, "max", 200));
                         juce::Array<juce::var> lines;
                         size_t skip = entries.size() > max ? entries.size() - max : 0;
                         for (size_t i = skip; i < entries.size(); ++i)
                             if (source.isEmpty() || entries[i].source == source)
                                 lines.add ("[" + entries[i].source + "] " + entries[i].text);
                         return ToolResult::ok (object ({ { "lines", lines }, { "next", (juce::int64) cursor } }));
                     } });
    }

    // --------------------------------------------------------------------------------------
    // console (UART4)

    void AutomationTools::addConsoleTools()
    {
        tools.add ({ "console_read",
                     "Read console (UART4) text since an offset. Escape sequences are removed.",
                     Schema().integer ("since", "absolute offset (default: last 4 KiB)").integer ("max_bytes", "default 65536"),
                     [this] (const juce::var& a)
                     {
                         auto end = session.consoleText.end();
                         auto from = args::has (a, "since") ? (juce::uint64) juce::jmax (0, args::integer (a, "since")) : (end > 4096 ? end - 4096 : 0);
                         auto chunk = session.consoleText.read (from, (size_t) juce::jmax (1, args::integer (a, "max_bytes", 65536)));
                         return ToolResult::ok (object ({ { "text", chunk.text }, { "from", (juce::int64) chunk.from },
                                                          { "next", (juce::int64) chunk.to }, { "truncated", chunk.truncated } }));
                     } });

        tools.add ({ "console_write",
                     "Type text on the console. Use \\n for Enter; escape sequences like \\u0003 (Ctrl-C) are sent as is.",
                     Schema().string ("text", "text to send", true).boolean ("enter", "append a newline (default false)"),
                     [this] (const juce::var& a)
                     {
                         if (! session.console.isConnected())
                             return ToolResult::fail ("console not connected (QEMU not running?)");
                         auto text = args::str (a, "text");
                         if (args::boolean (a, "enter"))
                             text << "\n";
                         session.console.sendText (text);
                         return ToolResult::ok (object ({ { "sent", (juce::int64) text.getNumBytesAsUTF8() }, { "offset", (juce::int64) session.consoleText.end() } }));
                     } });

        tools.add ({ "console_wait",
                     "Wait until the console prints text matching a regex (ECMAScript). Returns the match and the offset after it.",
                     Schema().string ("pattern", "regular expression", true).integer ("timeout_ms", "default 30000")
                             .integer ("since", "search output from this offset (default: current end, i.e. only new output)"),
                     [this] (const juce::var& a)
                     {
                         const auto from = args::has (a, "since") ? (juce::uint64) juce::jmax (0, args::integer (a, "since")) : session.consoleText.end();
                         juce::String error;
                         auto m = session.consoleText.waitFor (args::str (a, "pattern"), from, args::integer (a, "timeout_ms", 30000), error);
                         if (error.isNotEmpty())
                             return ToolResult::fail (error);
                         if (! m.found)
                         {
                             auto tail = session.consoleText.read (session.consoleText.end() > 600 ? session.consoleText.end() - 600 : 0);
                             auto r = ToolResult::fail ("timeout: pattern not seen");
                             r.error << "; last output:\n" << tail.text;
                             return r;
                         }
                         return ToolResult::ok (object ({ { "match", m.text }, { "line", m.context },
                                                          { "offset", (juce::int64) m.matchStart }, { "next", (juce::int64) m.matchEnd } }));
                     } });

        tools.add ({ "console_run",
                     "Run a shell command on the console and return its output (waits for the prompt).",
                     Schema().string ("command", "command line", true)
                             .string ("prompt", "regex for the prompt at the end of the output (default '[#$] $')")
                             .integer ("timeout_ms", "default 30000"),
                     [this] (const juce::var& a)
                     {
                         if (! session.console.isConnected())
                             return ToolResult::fail ("console not connected (QEMU not running?)");
                         const auto mark = session.consoleText.end();
                         const auto command = args::str (a, "command");
                         session.console.sendText (command + "\n");
                         juce::String error;
                         auto m = session.consoleText.waitFor (args::str (a, "prompt", "[#$] $"), mark + (juce::uint64) command.length() + 1,
                                                               args::integer (a, "timeout_ms", 30000), error);
                         if (error.isNotEmpty())
                             return ToolResult::fail (error);
                         auto all = session.consoleText.read (mark, 1 << 20).text;
                         auto output = all;
                         // Drop the echoed command line and the trailing prompt line.
                         if (output.startsWith (command))
                             output = output.fromFirstOccurrenceOf ("\n", false, false);
                         if (m.found)
                             output = output.upToLastOccurrenceOf ("\n", false, false);
                         if (! m.found)
                             return ToolResult::fail ("timeout waiting for the prompt; output so far:\n" + output);
                         return ToolResult::ok (object ({ { "output", output }, { "next", (juce::int64) m.matchEnd } }));
                     } });
    }

    // --------------------------------------------------------------------------------------
    // screen

    void AutomationTools::addScreenTools()
    {
        tools.add ({ "screenshot",
                     "Capture the 1024x600 panel as PNG (returned as an image; optionally also saved to a file).",
                     Schema().string ("path", "also write the PNG here"),
                     [this] (const juce::var& a)
                     {
                         auto& fb = session.framebuffer;
                         juce::Image copy;
                         {
                             std::lock_guard<std::mutex> guard (fb.lock);
                             if (fb.image.isValid())
                                 copy = fb.image.createCopy();
                         }
                         if (! copy.isValid())
                             return ToolResult::fail ("no picture yet (display not connected or guest has not set a mode)");
                         juce::MemoryOutputStream png;
                         juce::PNGImageFormat().writeImageToStream (copy, png);
                         ToolResult r = ToolResult::ok (object ({ { "width", copy.getWidth() }, { "height", copy.getHeight() },
                                                                  { "frame", (juce::int64) fb.serial.load() } }));
                         r.png = png.getMemoryBlock();
                         if (auto path = args::str (a, "path"); path.isNotEmpty())
                         {
                             auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
                             if (! file.replaceWithData (r.png.getData(), r.png.getSize()))
                                 return ToolResult::fail ("cannot write " + file.getFullPathName());
                             r.data.getDynamicObject()->setProperty ("path", file.getFullPathName());
                         }
                         return r;
                     } });

        tools.add ({ "screen_wait",
                     "Wait until the panel shows a new frame after 'frame' (from screenshot/status). Returns the new frame number.",
                     Schema().integer ("frame", "last frame seen", true).integer ("timeout_ms", "default 10000"),
                     [this] (const juce::var& a)
                     {
                         const auto last = (juce::uint32) args::integer (a, "frame");
                         const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) args::integer (a, "timeout_ms", 10000);
                         while (session.framebuffer.serial.load() == last)
                         {
                             if (juce::Time::getMillisecondCounter() > deadline)
                                 return ToolResult::fail ("no new frame");
                             sleepMs (20);
                         }
                         return ToolResult::ok (object ({ { "frame", (juce::int64) session.framebuffer.serial.load() } }));
                     } });
    }

    // --------------------------------------------------------------------------------------
    // front panel (MCU) and touch

    ToolResult AutomationTools::touchPhase (int slot, int phase, int x, int y)
    {
        // Same event sequence as ScreenComponent::sendContact (see host-interface.md).
        auto& cfg = session.config;
        const int absMax = cfg.getInt ("host.touch.abs_max", 0x7fff);
        const int w = juce::jmax (2, cfg.getInt ("panel.width", 1024)), h = juce::jmax (2, cfg.getInt ("panel.height", 600));
        const juce::Point<int> axes { juce::roundToInt (juce::jlimit (0, w - 1, x) * (double) absMax / (w - 1)),
                                      juce::roundToInt (juce::jlimit (0, h - 1, y) * (double) absMax / (h - 1)) };

        auto mtt = [] (const char* type, int s, int id, const char* axis, int value)
        {
            return object ({ { "type", "mtt" }, { "data", object ({ { "type", type }, { "slot", s }, { "tracking-id", id },
                                                                    { "axis", axis }, { "value", value } }) } });
        };
        auto btn = [] (bool down) { return object ({ { "type", "btn" }, { "data", object ({ { "button", "touch" }, { "down", down } }) } }); };

        juce::Array<juce::var> events;
        {
            std::lock_guard<std::mutex> guard (touchLock);
            if (phase == 0)
            {
                touching[(size_t) slot] = true;
                trackingIds[(size_t) slot] = nextTrackingId++ & 0xffff;
            }
            if (! touching[(size_t) slot])
                return ToolResult::fail ("contact " + juce::String (slot) + " is not down");
            touchAxes[(size_t) slot] = axes;
            for (int i = 0; i < (int) touching.size(); ++i)
            {
                if (! touching[(size_t) i])
                    continue;
                if (i == slot && phase == 2)
                {
                    events.add (mtt ("end", i, -1, "x", 0));
                    continue;
                }
                events.add (mtt (i == slot ? (phase == 0 ? "begin" : "update") : "update", i, trackingIds[(size_t) i], "x", 0));
                events.add (btn (true));
                events.add (mtt ("data", i, trackingIds[(size_t) i], "x", touchAxes[(size_t) i].x));
                events.add (mtt ("data", i, trackingIds[(size_t) i], "y", touchAxes[(size_t) i].y));
            }
            if (phase == 2)
                touching[(size_t) slot] = false;
            bool any = false;
            for (auto t : touching)
                any |= t;
            if (! any)
                events.add (btn (false));
        }
        return qmp ("input-send-event", object ({ { "events", events } }));
    }

    void AutomationTools::addPanelTools()
    {
        tools.add ({ "panel_controls",
                     "List the front-panel controls usable with press/turn/pad (id, type, label).",
                     Schema(),
                     [this] (const juce::var&)
                     {
                         juce::Array<juce::var> list;
                         for (auto& c : layout.controls)
                             if (c.type != "led" && c.type != "screen" && c.type != "pot")
                                 list.add (object ({ { "id", c.id }, { "type", c.type }, { "label", c.label.replace ("\n", " ") },
                                                     { "key", c.key } }));
                         return ToolResult::ok (object ({ { "controls", list } }));
                     } });

        tools.add ({ "press",
                     "Press a front-panel button, jog key or pad (by id from panel_controls or printed label). "
                     "action: tap (default), down, up, long (held past the long-press threshold).",
                     Schema().string ("control", "control id or label, e.g. play, shift, tap_tempo, pad5", true)
                             .enumeration ("action", { "tap", "down", "up", "long" }, "default tap")
                             .integer ("hold_ms", "tap/long hold time (default 80 / 1200)")
                             .integer ("velocity", "pads only, 1..127 (default 100)"),
                     [this] (const juce::var& a)
                     {
                         auto* c = control (args::str (a, "control"));
                         if (c == nullptr)
                             return ToolResult::fail ("unknown control: " + args::str (a, "control") + " (see panel_controls)");
                         if (session.mcu.protocol().getLinkState() == McuProtocol::LinkState::disconnected)
                             return ToolResult::fail ("MCU UART not connected (QEMU not running?)");

                         const auto action = args::str (a, "action", "tap");
                         auto& mcu = session.mcu.protocol();
                         std::function<void (bool)> set;
                         if (c->type == "pad")
                         {
                             const int velocity = juce::jlimit (1, 127, args::integer (a, "velocity", 100));
                             set = [&mcu, c, velocity] (bool down) { mcu.sendPad (c->hw, down ? DC::BTN_PRESSED : DC::BTN_RELEASED, down ? velocity : 0, 0); };
                         }
                         else if (c->type == "jog")
                         {
                             const int id = c->pushHw >= 0 ? c->pushHw : (int) DC::EN_KNOB_ENTER;
                             set = [&mcu, id] (bool down) { mcu.sendButton (id, down ? DC::BTN_PRESSED : DC::BTN_RELEASED); };
                         }
                         else if (c->hw >= 0)
                         {
                             const bool isShift = c->hw == DC::EN_SHIFT;
                             const int id = (! isShift && shiftHeld.load() && c->shiftHw >= 0) ? c->shiftHw : c->hw;
                             set = [this, &mcu, id, isShift] (bool down)
                             {
                                 if (isShift)
                                     shiftHeld = down;
                                 mcu.sendButton (id, down ? DC::BTN_PRESSED : DC::BTN_RELEASED);
                             };
                         }
                         else
                         {
                             return ToolResult::fail (c->id + " is not a button (use turn or slider)");
                         }

                         if (action == "down") set (true);
                         else if (action == "up") set (false);
                         else if (action == "long")
                         {
                             set (true);
                             sleepMs (args::integer (a, "hold_ms", 1200));
                             if (c->type != "pad" && c->hw >= 0)
                                 mcu.sendButton (c->hw, DC::BTN_LONGPRESSED);
                             set (false);
                         }
                         else
                         {
                             set (true);
                             sleepMs (args::integer (a, "hold_ms", 80));
                             set (false);
                         }
                         return ToolResult::ok (object ({ { "control", c->id }, { "action", action } }));
                     } });

        tools.add ({ "turn",
                     "Turn an encoder (p1..p4) or the jog wheel by signed steps (+ = clockwise).",
                     Schema().string ("control", "p1, p2, p3, p4 or jog", true).integer ("steps", "signed step count", true),
                     [this] (const juce::var& a)
                     {
                         auto* c = control (args::str (a, "control"));
                         if (c == nullptr || (c->type != "encoder" && c->type != "jog"))
                             return ToolResult::fail ("not an encoder: " + args::str (a, "control"));
                         const int steps = args::integer (a, "steps");
                         session.mcu.protocol().sendKnob (c->knob >= 0 ? c->knob : (int) DC::EN_KNOB_CENTER, steps);
                         return ToolResult::ok (object ({ { "control", c->id }, { "steps", steps } }));
                     } });

        tools.add ({ "slider",
                     "Set the touch slider position in percent (0..100).",
                     Schema().integer ("percent", "0..100", true),
                     [this] (const juce::var& a)
                     {
                         const int p = juce::jlimit (0, 100, args::integer (a, "percent"));
                         session.mcu.protocol().sendSlider (p);
                         return ToolResult::ok (object ({ { "percent", p } }));
                     } });

        tools.add ({ "panel_state",
                     "What the device shows: button LEDs, pad colours/states, slider LEDs, BPM and the MCU link.",
                     Schema(),
                     [this] (const juce::var&)
                     {
                         auto& st = session.panel;
                         juce::Array<juce::var> leds, pads;
                         for (auto& c : layout.controls)
                         {
                             if (c.led < 0 || ! juce::isPositiveAndBelow (c.led, PanelState::numLeds))
                                 continue;
                             auto& l = st.leds[(size_t) c.led];
                             static const char* modes[] = { "off", "on", "blink" };
                             leds.add (object ({ { "control", c.id }, { "led", c.led }, { "mode", modes[juce::jmin (2, (int) l.mode.load())] },
                                                 { "colour", l.colourId.load() } }));
                         }
                         static const char* padModes[] = { "off", "on", "sample_assigned", "sample_not_assigned", "on_blink", "off_blink" };
                         for (int i = 0; i < PanelState::numPads; ++i)
                         {
                             auto& p = st.pads[(size_t) i];
                             pads.add (object ({ { "pad", i + 1 }, { "mode", padModes[juce::jmin (5, (int) p.mode.load())] },
                                                 { "colour", p.colourId.load() }, { "level", p.level.load() }, { "pressed", p.pressed.load() } }));
                         }
                         return ToolResult::ok (object ({ { "leds", leds }, { "pads", pads },
                                                          { "slider_percent", st.sliderPercent.load() }, { "bpm", st.bpm.load() },
                                                          { "host_linked", st.hostLinked.load() },
                                                          { "sw_version", juce::String (st.swVersionMajor.load()) + "." + juce::String (st.swVersionMinor.load()) } }));
                     } });

        auto touchTool = [this] (const juce::var& a, int phase)
        {
            const int slot = juce::jlimit (0, 9, args::integer (a, "slot", 0));
            return touchPhase (slot, phase, args::integer (a, "x"), args::integer (a, "y"));
        };

        tools.add ({ "touch",
                     "Touch the screen at pixel (x, y) of the 1024x600 panel. action: tap (default), down, move, up. "
                     "slot selects the finger for multitouch.",
                     Schema().integer ("x", "0..1023", true).integer ("y", "0..599", true)
                             .enumeration ("action", { "tap", "down", "move", "up" }, "default tap")
                             .integer ("slot", "finger 0..9 (default 0)").integer ("hold_ms", "tap hold (default 60)"),
                     [touchTool] (const juce::var& a)
                     {
                         const auto action = args::str (a, "action", "tap");
                         if (action == "down") return touchTool (a, 0);
                         if (action == "move") return touchTool (a, 1);
                         if (action == "up") return touchTool (a, 2);
                         auto r = touchTool (a, 0);
                         if (r.failed())
                             return r;
                         sleepMs (args::integer (a, "hold_ms", 60));
                         return touchTool (a, 2);
                     } });

        tools.add ({ "swipe",
                     "Drag one finger from (x1, y1) to (x2, y2) over duration_ms.",
                     Schema().integer ("x1", "", true).integer ("y1", "", true).integer ("x2", "", true).integer ("y2", "", true)
                             .integer ("duration_ms", "default 300"),
                     [this] (const juce::var& a)
                     {
                         const int x1 = args::integer (a, "x1"), y1 = args::integer (a, "y1"), x2 = args::integer (a, "x2"), y2 = args::integer (a, "y2");
                         const int duration = juce::jmax (20, args::integer (a, "duration_ms", 300));
                         const int steps = juce::jmax (2, duration / 16);
                         auto r = touchPhase (0, 0, x1, y1);
                         for (int i = 1; i <= steps && ! r.failed(); ++i)
                         {
                             sleepMs (duration / steps);
                             r = touchPhase (0, 1, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
                         }
                         if (r.failed())
                             return r;
                         return touchPhase (0, 2, x2, y2);
                     } });
    }

    // --------------------------------------------------------------------------------------
    // MIDI and CV

    void AutomationTools::addIoTools()
    {
        tools.add ({ "midi_send",
                     "Send raw MIDI bytes into the KICK's MIDI IN jack, e.g. [144, 60, 100] or \"90 3c 64\".",
                     Schema().string ("bytes", "hex string, or pass an array of numbers", true),
                     [this] (const juce::var& a)
                     {
                         std::vector<juce::uint8> bytes;
                         juce::String error;
                         if (! parseBytes (a["bytes"], bytes, error))
                             return ToolResult::fail (error);
                         if (! session.midi.sendToGuest (bytes.data(), bytes.size()))
                             return ToolResult::fail ("MIDI UART not connected (QEMU not running?)");
                         return ToolResult::ok (object ({ { "sent", toHex (bytes) } }));
                     } });

        tools.add ({ "midi_read",
                     "MIDI messages the KICK sent on MIDI OUT since an index.",
                     Schema().integer ("since", "first message index (from a previous 'next')").integer ("max", "default 256"),
                     [this] (const juce::var& a)
                     {
                         auto events = session.midiSince ((juce::uint64) juce::jmax (0, args::integer (a, "since")), (size_t) juce::jmax (1, args::integer (a, "max", 256)));
                         juce::Array<juce::var> list;
                         for (auto& e : events)
                             list.add (object ({ { "index", (juce::int64) e.index }, { "time", e.time }, { "bytes", toHex (e.bytes) } }));
                         return ToolResult::ok (object ({ { "messages", list },
                                                          { "next", (juce::int64) (events.empty() ? session.midiEnd() : events.back().index + 1) } }));
                     } });

        tools.add ({ "cv_read",
                     "CV/Gate outputs (volts the guest wrote to the AD5671R) and the input voltages set on the host.",
                     Schema(),
                     [this] (const juce::var&)
                     {
                         juce::Array<juce::var> out, in;
                         for (int ch = 0; ch < session.cv.getNumChannels(); ++ch)
                         {
                             out.add (object ({ { "channel", ch }, { "volts", session.cv.getOutputVolts (ch) }, { "raw", session.cv.getOutputRaw (ch) } }));
                             in.add (object ({ { "channel", ch }, { "volts", session.cv.getInputVolts (ch) } }));
                         }
                         return ToolResult::ok (object ({ { "connected", session.cv.isConnected() }, { "outputs", out }, { "inputs", in } }));
                     } });

        tools.add ({ "cv_set",
                     "Set the voltage the guest reads on a CV channel.",
                     Schema().integer ("channel", "channel index", true).number ("volts", "volts", true),
                     [this] (const juce::var& a)
                     {
                         const int ch = args::integer (a, "channel");
                         if (! juce::isPositiveAndBelow (ch, session.cv.getNumChannels()))
                             return ToolResult::fail ("channel out of range");
                         session.cv.setInputVolts (ch, (float) args::number (a, "volts"));
                         return ToolResult::ok (object ({ { "channel", ch }, { "volts", session.cv.getInputVolts (ch) } }));
                     } });
    }

    // --------------------------------------------------------------------------------------
    // SD slot, audio files, USB (contract: Emulator/docs/host-interface.md)

    void AutomationTools::addMediaTools()
    {
        tools.add ({ "sd_insert",
                     "Insert an SD card image (raw) into the front slot while the machine runs.",
                     Schema().string ("path", "image file", true),
                     [this] (const juce::var& a)
                     {
                         auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args::str (a, "path"));
                         if (! file.existsAsFile())
                             return ToolResult::fail ("no such file: " + file.getFullPathName());
                         return qmp ("blockdev-change-medium", object ({ { "device", "sd" }, { "filename", file.getFullPathName() }, { "format", "raw" } }));
                     } });

        tools.add ({ "sd_eject", "Pull the SD card out of the slot.", Schema(),
                     [this] (const juce::var&) { return qmp ("eject", object ({ { "device", "sd" }, { "force", true } })); } });

        tools.add ({ "sd_status", "Whether a card is in the SD slot and which image.", Schema(),
                     [this] (const juce::var&)
                     {
                         auto r = qmp ("query-block", {});
                         if (r.failed())
                             return r;
                         if (auto* list = r.data.getArray())
                             for (auto& b : *list)
                                 if (b["device"].toString() == "sd")
                                     return ToolResult::ok (object ({ { "inserted", b.hasProperty ("inserted") },
                                                                      { "image", b["inserted"]["file"] } }));
                         return ToolResult::fail ("the machine has no SD drive");
                     } });

        auto qomSet = [this] (const juce::String& property, const juce::var& value)
        {
            return qmp ("qom-set", object ({ { "path", "/machine" }, { "property", property }, { "value", value } }));
        };

        tools.add ({ "audio_wav_in",
                     "Feed a 48 kHz WAV file into the audio input (replaces the host input). Empty path stops it.",
                     Schema().string ("path", "WAV file, or empty to stop", true).boolean ("loop", "repeat the file (default false)"),
                     [qomSet] (const juce::var& a)
                     {
                         auto path = args::str (a, "path");
                         if (path.isNotEmpty())
                             path = juce::File::getCurrentWorkingDirectory().getChildFile (path).getFullPathName();
                         auto r = qomSet ("audio-wav-in-loop", args::boolean (a, "loop"));
                         return r.failed() ? r : qomSet ("audio-wav-in", path);
                     } });

        tools.add ({ "audio_wav_out",
                     "Record the audio output to a WAV file (48 kHz, 32-bit stereo). Empty path stops and finalises the file.",
                     Schema().string ("path", "WAV file, or empty to stop", true),
                     [qomSet] (const juce::var& a)
                     {
                         auto path = args::str (a, "path");
                         if (path.isNotEmpty())
                             path = juce::File::getCurrentWorkingDirectory().getChildFile (path).getFullPathName();
                         return qomSet ("audio-wav-out", path);
                     } });

        tools.add ({ "audio_status", "Audio frame counters and the active WAV files.", Schema(),
                     [this] (const juce::var&)
                     {
                         auto r = qmp ("qom-get", object ({ { "path", "/machine" }, { "property", "audio-status" } }));
                         if (! r.failed())
                         {
                             juce::var parsed;
                             if (juce::JSON::parse (r.data.toString(), parsed).wasOk())
                                 r.data = parsed;
                         }
                         return r;
                     } });

        tools.add ({ "usb_attach",
                     "Plug a virtual device into a USB port: storage (raw image), keyboard, mouse, tablet, or host "
                     "(pass through a host device by vendor/product id).",
                     Schema().enumeration ("device", { "storage", "keyboard", "mouse", "tablet", "host" }, "device kind", true)
                             .enumeration ("port", { "usb-a", "usb-c" }, "default usb-a")
                             .string ("path", "storage: image file").string ("vendorid", "host: e.g. 0x1235")
                             .string ("productid", "host: e.g. 0x8202").string ("id", "device id for usb_detach (default generated)"),
                     [this] (const juce::var& a)
                     {
                         static std::atomic<int> counter { 0 };
                         const auto kind = args::str (a, "device");
                         const auto bus = args::str (a, "port", "usb-a") + ".0";
                         const auto id = args::str (a, "id", "usb" + juce::String (++counter));
                         auto dev = object ({ { "bus", bus }, { "id", id } });
                         auto* o = dev.getDynamicObject();

                         if (kind == "storage")
                         {
                             auto file = juce::File::getCurrentWorkingDirectory().getChildFile (args::str (a, "path"));
                             if (! file.existsAsFile())
                                 return ToolResult::fail ("no such file: " + file.getFullPathName());
                             auto blk = qmp ("blockdev-add", object ({ { "driver", "raw" }, { "node-name", id + "-blk" },
                                                                        { "file", object ({ { "driver", "file" }, { "filename", file.getFullPathName() } }) } }));
                             if (blk.failed())
                                 return blk;
                             o->setProperty ("driver", "usb-storage");
                             o->setProperty ("drive", id + "-blk");
                         }
                         else if (kind == "keyboard") o->setProperty ("driver", "usb-kbd");
                         else if (kind == "mouse") o->setProperty ("driver", "usb-mouse");
                         else if (kind == "tablet") o->setProperty ("driver", "usb-tablet");
                         else if (kind == "host")
                         {
                             o->setProperty ("driver", "usb-host");
                             o->setProperty ("vendorid", args::str (a, "vendorid").getHexValue32());
                             o->setProperty ("productid", args::str (a, "productid").getHexValue32());
                         }
                         else
                         {
                             return ToolResult::fail ("unknown device kind: " + kind);
                         }
                         auto r = qmp ("device_add", dev);
                         if (r.failed())
                             return r;
                         return ToolResult::ok (object ({ { "id", id }, { "bus", bus } }));
                     } });

        tools.add ({ "usb_detach", "Unplug a USB device by the id usb_attach returned.",
                     Schema().string ("id", "device id", true),
                     [this] (const juce::var& a)
                     {
                         const auto id = args::str (a, "id");
                         auto r = qmp ("device_del", object ({ { "id", id } }));
                         if (r.failed())
                             return r;
                         // A storage stick also has a block node; it can go once the device is gone.
                         sleepMs (300);
                         qmp ("blockdev-del", object ({ { "node-name", id + "-blk" } }));
                         return ToolResult::ok (object ({ { "id", id } }));
                     } });

        tools.add ({ "usb_list", "Devices on the emulated USB buses.", Schema(),
                     [this] (const juce::var&)
                     {
                         auto r = qmp ("human-monitor-command", object ({ { "command-line", "info usb" } }));
                         if (! r.failed())
                             r.data = object ({ { "devices", r.data.toString() } });
                         return r;
                     } });
    }
}
