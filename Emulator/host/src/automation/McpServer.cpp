#include "automation/McpServer.h"

#include <string>

namespace kickemu
{
    McpServer::McpServer (const ToolRegistry& t, juce::String n, juce::String v, juce::String i)
        : tools (t), name (std::move (n)), version (std::move (v)), instructions (std::move (i)) {}

    juce::var toMcpContent (const ToolResult& result)
    {
        juce::Array<juce::var> content;
        auto text = result.failed() ? result.error : juce::JSON::toString (result.data, true);
        content.add (object ({ { "type", "text" }, { "text", text } }));
        if (result.png.getSize() > 0)
            content.add (object ({ { "type", "image" },
                                   { "data", juce::Base64::toBase64 (result.png.getData(), result.png.getSize()) },
                                   { "mimeType", "image/png" } }));
        return content;
    }

    juce::var McpServer::dispatch (const juce::String& method, const juce::var& params, bool& isError, int& code, juce::String& message)
    {
        if (method == "initialize")
        {
            // Answer with the client's version when we know it, else our latest.
            auto requested = params["protocolVersion"].toString();
            static const juce::StringArray known { "2024-11-05", "2025-03-26", "2025-06-18" };
            auto info = object ({ { "name", name }, { "version", version } });
            auto caps = object ({ { "tools", object ({ { "listChanged", false } }) } });
            auto result = object ({ { "protocolVersion", known.contains (requested) ? requested : juce::String (latestProtocol) },
                                    { "capabilities", caps },
                                    { "serverInfo", info } });
            if (instructions.isNotEmpty())
                result.getDynamicObject()->setProperty ("instructions", instructions);
            return result;
        }
        if (method == "ping")
            return juce::var (new juce::DynamicObject());
        if (method == "tools/list")
        {
            juce::Array<juce::var> list;
            for (auto& t : tools.all())
                list.add (object ({ { "name", t.name }, { "description", t.description }, { "inputSchema", t.inputSchema } }));
            return object ({ { "tools", list } });
        }
        if (method == "tools/call")
        {
            auto toolName = params["name"].toString();
            if (tools.find (toolName) == nullptr)
            {
                isError = true;
                code = -32602;
                message = "Unknown tool: " + toolName;
                return {};
            }
            auto result = tools.call (toolName, params["arguments"]);
            return object ({ { "content", toMcpContent (result) }, { "isError", result.failed() } });
        }
        isError = true;
        code = -32601;
        message = "Method not found: " + method;
        return {};
    }

    std::optional<juce::String> McpServer::handle (const juce::String& line)
    {
        auto reply = [] (const juce::var& id, const juce::var& result) -> juce::String
        {
            return juce::JSON::toString (object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }), true);
        };
        auto failure = [] (const juce::var& id, int code, const juce::String& message) -> juce::String
        {
            return juce::JSON::toString (object ({ { "jsonrpc", "2.0" }, { "id", id },
                                                   { "error", object ({ { "code", code }, { "message", message } }) } }), true);
        };

        juce::var msg;
        if (auto r = juce::JSON::parse (line, msg); r.failed() || ! msg.isObject())
            return failure ({}, -32700, "Parse error");

        const auto method = msg["method"].toString();
        const bool isNotification = ! msg.hasProperty ("id");
        if (method.isEmpty())
            return isNotification ? std::nullopt : std::optional<juce::String> (failure (msg["id"], -32600, "Invalid request"));
        if (isNotification)
            return std::nullopt;

        bool isError = false;
        int code = 0;
        juce::String message;
        auto result = dispatch (method, msg["params"], isError, code, message);
        return isError ? failure (msg["id"], code, message) : reply (msg["id"], result);
    }

    void McpServer::serve (std::FILE* in, std::FILE* out, const std::atomic<bool>& stop)
    {
        std::string line;
        int c;
        while (! stop.load() && (c = std::fgetc (in)) != EOF)
        {
            if (c != '\n')
            {
                line.push_back ((char) c);
                continue;
            }
            auto text = juce::String::fromUTF8 (line.data(), (int) line.size()).trim();
            line.clear();
            if (text.isEmpty())
                continue;
            if (auto response = handle (text))
            {
                std::fputs (response->toRawUTF8(), out);
                std::fputc ('\n', out);
                std::fflush (out);
            }
        }
    }
}
