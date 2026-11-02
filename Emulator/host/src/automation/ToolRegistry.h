#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace kickemu
{
    /** Result of one automation tool call: structured data, an error, or a PNG image. */
    struct ToolResult
    {
        juce::var data;              // JSON-serialisable result
        juce::String error;          // non-empty = the call failed
        juce::MemoryBlock png;       // optional image (screenshots)

        static ToolResult fail (const juce::String& message) { ToolResult r; r.error = message; return r; }
        static ToolResult ok (juce::var value = juce::var (new juce::DynamicObject())) { ToolResult r; r.data = std::move (value); return r; }
        bool failed() const { return error.isNotEmpty(); }
    };

    /**
        The automation surface of the emulator: named tools with a JSON-schema for
        their arguments. The same registry is served over MCP (stdio) and over the
        JSON-RPC automation socket, so agents, CI scripts and tests drive the
        emulator through one implementation. Handlers may be called from any thread.
    */
    class ToolRegistry
    {
    public:
        struct Tool
        {
            juce::String name, description;
            juce::var inputSchema;   // JSON schema object
            std::function<ToolResult (const juce::var& args)> run;
        };

        void add (Tool tool);
        const Tool* find (const juce::String& name) const;
        const std::vector<Tool>& all() const { return tools; }

        /** Calls a tool; unknown names and missing required arguments are errors. */
        ToolResult call (const juce::String& name, const juce::var& args) const;

    private:
        std::vector<Tool> tools;
    };

    /** Small builder for the JSON schemas of tool arguments. */
    class Schema
    {
    public:
        Schema& string (const juce::String& name, const juce::String& description, bool required = false);
        Schema& integer (const juce::String& name, const juce::String& description, bool required = false);
        Schema& number (const juce::String& name, const juce::String& description, bool required = false);
        Schema& boolean (const juce::String& name, const juce::String& description, bool required = false);
        Schema& enumeration (const juce::String& name, const juce::StringArray& values, const juce::String& description, bool required = false);
        Schema& array (const juce::String& name, const juce::String& itemType, const juce::String& description, bool required = false);
        operator juce::var() const;

    private:
        Schema& add (const juce::String& name, juce::DynamicObject* prop, const juce::String& description, bool required);
        juce::DynamicObject::Ptr props { new juce::DynamicObject() };
        juce::Array<juce::var> required;
    };

    /** Helpers for reading tool arguments. */
    namespace args
    {
        bool has (const juce::var& a, const char* name);
        juce::String str (const juce::var& a, const char* name, const juce::String& fallback = {});
        int integer (const juce::var& a, const char* name, int fallback = 0);
        double number (const juce::var& a, const char* name, double fallback = 0.0);
        bool boolean (const juce::var& a, const char* name, bool fallback = false);
    }

    /** {"key": value, ...} from an initializer list. */
    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields);
}
