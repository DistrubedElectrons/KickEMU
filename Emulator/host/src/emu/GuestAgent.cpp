#include "emu/GuestAgent.h"

namespace kickemu
{
    namespace
    {
        constexpr int chunkBytes = 48 * 1024;     // per guest-file-write/read (64 KiB as base64)

        juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
        {
            auto* o = new juce::DynamicObject();
            for (auto& p : props)
                o->setProperty (p.first, p.second);
            return juce::var (o);
        }

        juce::String base64 (const void* data, size_t size)
        {
            return juce::Base64::toBase64 (data, size);
        }

        bool fromBase64 (const juce::String& text, juce::MemoryBlock& out)
        {
            juce::MemoryOutputStream stream (out, true);
            return juce::Base64::convertFromBase64 (stream, text);
        }
    }

    GuestAgent::GuestAgent (ConsoleBridge& u)
        : uart (u), nextSyncId ((juce::int64) juce::Random::getSystemRandom().nextInt (1 << 30))
    {
        uart.onRawOutput = [this] (const juce::uint8* d, size_t n) { received (d, n); };
    }

    GuestAgent::~GuestAgent()
    {
        uart.onRawOutput = nullptr;
    }

    void GuestAgent::received (const juce::uint8* data, size_t size)
    {
        std::lock_guard<std::mutex> g (rxLock);
        for (size_t i = 0; i < size; ++i)
        {
            const auto c = (char) data[i];
            if (data[i] == 0xff)            // sync delimiter: drop what came before it
            {
                partial.clear();
                continue;
            }
            if (c != '\n')
            {
                if (partial.size() < (64u << 20))
                    partial.push_back (c);
                continue;
            }
            auto text = juce::String::fromUTF8 (partial.data(), (int) partial.size()).trim();
            partial.clear();
            if (text.isEmpty())
                continue;
            auto parsed = juce::JSON::parse (text);
            if (parsed.isObject())
            {
                lines.push_back (parsed);
                answered = true;
            }
        }
        rxReady.notify_all();
    }

    bool GuestAgent::waitForLine (juce::var& out, int timeoutMs)
    {
        std::unique_lock<std::mutex> g (rxLock);
        if (! rxReady.wait_for (g, std::chrono::milliseconds (timeoutMs), [this] { return ! lines.empty(); }))
            return false;
        out = lines.front();
        lines.pop_front();
        return true;
    }

    juce::Result GuestAgent::sync (int timeoutMs)
    {
        if (! uart.isConnected())
            return juce::Result::fail ("UART2 is not connected (QEMU not running?)");
        {
            std::lock_guard<std::mutex> g (rxLock);
            lines.clear();
            partial.clear();
        }
        const auto id = ++nextSyncId;
        const juce::uint8 ff = 0xff;
        uart.sendBytes (&ff, 1);
        uart.sendText (juce::JSON::toString (object ({ { "execute", "guest-sync-delimited" },
                                                       { "arguments", object ({ { "id", id } }) } }), true) + "\n");
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        for (;;)
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now >= deadline)
                return juce::Result::fail ("no guest agent on UART2 (is qemu-ga running on /dev/ttymxc1?)");
            juce::var line;
            if (waitForLine (line, (int) (deadline - now)) && (juce::int64) line["return"] == id)
                return juce::Result::ok();
        }
    }

    juce::var GuestAgent::call (const juce::String& command, const juce::var& arguments, int timeoutMs, juce::String& error)
    {
        auto request = object ({ { "execute", command } });
        if (! arguments.isVoid())
            request.getDynamicObject()->setProperty ("arguments", arguments);
        uart.sendText (juce::JSON::toString (request, true) + "\n");
        juce::var line;
        if (! waitForLine (line, timeoutMs))
        {
            error = command + ": no answer within " + juce::String (timeoutMs) + " ms";
            return {};
        }
        if (line.hasProperty ("error"))
        {
            error = command + ": " + line["error"]["desc"].toString();
            return {};
        }
        return line["return"];
    }

    namespace
    {
        /** Keeps agent traffic out of the UART2 terminal for one operation. */
        struct QuietTerminal
        {
            explicit QuietTerminal (ConsoleBridge& b) : bridge (b) { bridge.setTerminalEnabled (false); }
            ~QuietTerminal() { bridge.setTerminalEnabled (true); }
            ConsoleBridge& bridge;
        };
    }

    juce::Result GuestAgent::ping (juce::String& version, int timeoutMs)
    {
        std::lock_guard<std::mutex> op (opLock);
        QuietTerminal quiet (uart);
        if (auto r = sync (timeoutMs); r.failed())
            return r;
        juce::String error;
        auto info = call ("guest-info", {}, timeoutMs, error);
        if (error.isNotEmpty())
            return juce::Result::fail (error);
        version = info["version"].toString();
        return juce::Result::ok();
    }

    juce::Result GuestAgent::push (const juce::File& local, const juce::String& remote, int mode,
                                   std::function<void (juce::int64, juce::int64)> progress)
    {
        juce::FileInputStream in (local);
        if (! in.openedOk())
            return juce::Result::fail ("cannot read " + local.getFullPathName());

        std::lock_guard<std::mutex> op (opLock);
        QuietTerminal quiet (uart);
        if (auto r = sync (3000); r.failed())
            return r;
        juce::String error;
        auto handle = call ("guest-file-open", object ({ { "path", remote }, { "mode", "wb" } }), 5000, error);
        if (error.isNotEmpty())
            return juce::Result::fail (error);

        const auto total = in.getTotalLength();
        juce::HeapBlock<char> buffer (chunkBytes);
        juce::int64 done = 0;
        while (error.isEmpty() && ! in.isExhausted())
        {
            const int n = in.read (buffer, chunkBytes);
            if (n <= 0)
                break;
            auto w = call ("guest-file-write", object ({ { "handle", handle }, { "buf-b64", base64 (buffer, (size_t) n) } }),
                           15000, error);
            if (error.isEmpty() && (int) w["count"] != n)
                error = "short write in the guest";
            done += n;
            if (progress)
                progress (done, total);
        }
        juce::String closeError;
        call ("guest-file-close", object ({ { "handle", handle } }), 5000, closeError);
        if (error.isEmpty())
            error = closeError;
        if (error.isEmpty() && mode >= 0)
        {
            // guest-file-open creates 0666 & ~umask and there is no chmod command: run one.
            auto pid = call ("guest-exec", object ({ { "path", "/bin/chmod" },
                                                     { "arg", juce::StringArray { juce::String::formatted ("%o", mode), remote } },
                                                     { "capture-output", true } }), 5000, error);
            for (int i = 0; error.isEmpty() && i < 100; ++i)
            {
                auto st = call ("guest-exec-status", object ({ { "pid", pid["pid"] } }), 5000, error);
                if ((bool) st["exited"])
                {
                    if ((int) st["exitcode"] != 0)
                        error = "chmod failed in the guest";
                    break;
                }
                juce::Thread::sleep (20);
            }
        }
        return error.isEmpty() ? juce::Result::ok() : juce::Result::fail (error);
    }

    juce::Result GuestAgent::pull (const juce::String& remote, const juce::File& local, juce::int64& bytes)
    {
        std::lock_guard<std::mutex> op (opLock);
        QuietTerminal quiet (uart);
        if (auto r = sync (3000); r.failed())
            return r;
        juce::String error;
        auto handle = call ("guest-file-open", object ({ { "path", remote }, { "mode", "rb" } }), 5000, error);
        if (error.isEmpty())
        {
            local.getParentDirectory().createDirectory();
            local.deleteFile();
            juce::FileOutputStream out (local);
            if (! out.openedOk())
                error = "cannot write " + local.getFullPathName();
            bytes = 0;
            while (error.isEmpty())
            {
                auto r = call ("guest-file-read", object ({ { "handle", handle }, { "count", chunkBytes } }), 15000, error);
                if (error.isNotEmpty())
                    break;
                juce::MemoryBlock data;
                if (! fromBase64 (r["buf-b64"].toString(), data))
                    error = "bad base64 from the guest";
                out.write (data.getData(), data.getSize());
                bytes += (juce::int64) data.getSize();
                if ((bool) r["eof"] || data.getSize() == 0)
                    break;
            }
            juce::String closeError;
            call ("guest-file-close", object ({ { "handle", handle } }), 5000, closeError);
        }
        return error.isEmpty() ? juce::Result::ok() : juce::Result::fail (error);
    }

    juce::Result GuestAgent::exec (const juce::String& path, const juce::StringArray& args, const juce::MemoryBlock& input,
                                   int timeoutMs, ExecResult& result)
    {
        std::lock_guard<std::mutex> op (opLock);
        QuietTerminal quiet (uart);
        if (auto r = sync (3000); r.failed())
            return r;
        juce::String error;
        auto request = object ({ { "path", path }, { "arg", args }, { "capture-output", true } });
        if (input.getSize() > 0)
            request.getDynamicObject()->setProperty ("input-data", base64 (input.getData(), input.getSize()));
        auto started = call ("guest-exec", request, 5000, error);
        if (error.isNotEmpty())
            return juce::Result::fail (error);
        result.pid = (juce::int64) started["pid"];

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        int pollMs = 10;
        for (;;)
        {
            auto st = call ("guest-exec-status", object ({ { "pid", result.pid } }), 5000, error);
            if (error.isNotEmpty())
                return juce::Result::fail (error);
            if ((bool) st["exited"])
            {
                result.exitCode = st.hasProperty ("exitcode") ? (int) st["exitcode"] : -1;
                result.signal = st.hasProperty ("signal") ? (int) st["signal"] : 0;
                fromBase64 (st["out-data"].toString(), result.out);
                fromBase64 (st["err-data"].toString(), result.err);
                return juce::Result::ok();
            }
            if (juce::Time::getMillisecondCounter() >= deadline)
            {
                result.timedOut = true;
                return juce::Result::ok();
            }
            juce::Thread::sleep (pollMs);
            pollMs = juce::jmin (200, pollMs * 2);
        }
    }
}
