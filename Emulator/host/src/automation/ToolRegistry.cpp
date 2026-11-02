#include "automation/ToolRegistry.h"

namespace kickemu
{
    void ToolRegistry::add (Tool tool)
    {
        jassert (find (tool.name) == nullptr);
        tools.push_back (std::move (tool));
    }

    const ToolRegistry::Tool* ToolRegistry::find (const juce::String& name) const
    {
        for (auto& t : tools)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    ToolResult ToolRegistry::call (const juce::String& name, const juce::var& a) const
    {
        auto* tool = find (name);
        if (tool == nullptr)
            return ToolResult::fail ("Unknown tool: " + name);

        const auto callArgs = a.isObject() ? a : juce::var (new juce::DynamicObject());
        if (auto* req = tool->inputSchema["required"].getArray())
            for (auto& r : *req)
                if (! callArgs.hasProperty (r.toString()))
                    return ToolResult::fail ("Missing argument: " + r.toString());

        try
        {
            return tool->run (callArgs);
        }
        catch (const std::exception& e)
        {
            return ToolResult::fail (name + " failed: " + e.what());
        }
    }

    // ------------------------------------------------------------------------------------

    Schema& Schema::add (const juce::String& name, juce::DynamicObject* prop, const juce::String& description, bool isRequired)
    {
        prop->setProperty ("description", description);
        props->setProperty (name, juce::var (prop));
        if (isRequired)
            required.add (name);
        return *this;
    }

    static juce::DynamicObject* typed (const char* type)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", type);
        return o;
    }

    Schema& Schema::string (const juce::String& n, const juce::String& d, bool r) { return add (n, typed ("string"), d, r); }
    Schema& Schema::integer (const juce::String& n, const juce::String& d, bool r) { return add (n, typed ("integer"), d, r); }
    Schema& Schema::number (const juce::String& n, const juce::String& d, bool r) { return add (n, typed ("number"), d, r); }
    Schema& Schema::boolean (const juce::String& n, const juce::String& d, bool r) { return add (n, typed ("boolean"), d, r); }

    Schema& Schema::enumeration (const juce::String& n, const juce::StringArray& values, const juce::String& d, bool r)
    {
        auto* o = typed ("string");
        juce::Array<juce::var> list;
        for (auto& v : values)
            list.add (v);
        o->setProperty ("enum", list);
        return add (n, o, d, r);
    }

    Schema& Schema::array (const juce::String& n, const juce::String& itemType, const juce::String& d, bool r)
    {
        auto* o = typed ("array");
        o->setProperty ("items", juce::var (typed (itemType.toRawUTF8())));
        return add (n, o, d, r);
    }

    Schema::operator juce::var() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", "object");
        o->setProperty ("properties", juce::var (props->clone().release()));
        if (! required.isEmpty())
            o->setProperty ("required", required);
        return juce::var (o);
    }

    // ------------------------------------------------------------------------------------

    namespace args
    {
        bool has (const juce::var& a, const char* name) { return a.hasProperty (name) && ! a[name].isVoid(); }
        juce::String str (const juce::var& a, const char* name, const juce::String& fallback) { return has (a, name) ? a[name].toString() : fallback; }
        int integer (const juce::var& a, const char* name, int fallback) { return has (a, name) ? (int) a[name] : fallback; }
        double number (const juce::var& a, const char* name, double fallback) { return has (a, name) ? (double) a[name] : fallback; }

        bool boolean (const juce::var& a, const char* name, bool fallback)
        {
            if (! has (a, name))
                return fallback;
            auto v = a[name];
            if (v.isString())
                return v.toString().equalsIgnoreCase ("true") || v.toString() == "1";
            return (bool) v;
        }
    }

    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (auto& f : fields)
            o->setProperty (f.first, f.second);
        return juce::var (o);
    }
}
