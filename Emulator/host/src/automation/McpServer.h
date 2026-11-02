#pragma once

#include "automation/ToolRegistry.h"

#include <atomic>
#include <cstdio>
#include <optional>

namespace kickemu
{
    /**
        Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).

        Supports initialize, ping, tools/list and tools/call; notifications are
        accepted and ignored. Tool results are returned as one text block holding
        the JSON result, plus an image block for screenshots. While serving, stdout
        belongs to the protocol: the caller must route its logging to stderr.
    */
    class McpServer
    {
    public:
        McpServer (const ToolRegistry& tools, juce::String name, juce::String version, juce::String instructions = {});

        /** Handles one JSON-RPC message; returns the response line, or nothing for
            notifications. Public for tests. */
        std::optional<juce::String> handle (const juce::String& line);

        /** Reads requests from `in` until EOF or `stop` is set; writes responses to `out`.
            Requests are answered in order (a slow tool blocks the next request). */
        void serve (std::FILE* in, std::FILE* out, const std::atomic<bool>& stop);

        static constexpr const char* latestProtocol = "2025-06-18";

    private:
        juce::var dispatch (const juce::String& method, const juce::var& params, bool& isError, int& errorCode, juce::String& errorMessage);

        const ToolRegistry& tools;
        juce::String name, version, instructions;
    };

    /** Converts a tool result into MCP `content` blocks. */
    juce::var toMcpContent (const ToolResult& result);
}
