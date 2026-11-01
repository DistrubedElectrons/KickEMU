#include "config/Config.h"

namespace kickemu
{
    namespace
    {
        // Walks a dotted path through nested objects; returns nullptr when missing.
        const juce::var* lookup (const juce::var& root, const juce::String& path)
        {
            const juce::var* node = &root;
            for (auto& part : juce::StringArray::fromTokens (path, ".", {}))
            {
                auto* obj = node->getDynamicObject();
                if (obj == nullptr || ! obj->hasProperty (part))
                    return nullptr;
                node = obj->getProperties().getVarPointer (part);
            }
            return node;
        }
    }

    juce::Result Config::load (const juce::File& configFile)
    {
        if (! configFile.existsAsFile())
            return juce::Result::fail ("Config file not found: " + configFile.getFullPathName());

        auto root_ = findRepoRoot (configFile.getParentDirectory());
        if (! root_.isDirectory())
            root_ = configFile.getParentDirectory();

        auto result = loadFromString (configFile.loadFileAsString(), root_);
        if (result.wasOk())
            file = configFile;
        return result;
    }

    juce::Result Config::loadFromString (const juce::String& json, const juce::File& repoRootDir)
    {
        juce::var parsed;
        auto result = juce::JSON::parse (json, parsed);
        if (result.failed())
            return juce::Result::fail ("Config is not valid JSON: " + result.getErrorMessage());
        if (! parsed.isObject())
            return juce::Result::fail ("Config root must be a JSON object");

        root = parsed.clone();
        repoRoot = repoRootDir;

        // An explicit repo_root in the file wins over the detected one.
        if (auto explicitRoot = getString ("repo_root"); explicitRoot.isNotEmpty())
            repoRoot = juce::File::getCurrentWorkingDirectory().getChildFile (expandEnvironment (explicitRoot));

        return juce::Result::ok();
    }

    juce::Result Config::applyProfile (const juce::String& name)
    {
        auto* profile = lookup (root, "profiles." + name);
        if (profile == nullptr || ! profile->isObject())
            return juce::Result::fail ("Unknown profile: " + name + " (available: " + getProfileNames().joinIntoString (", ") + ")");

        deepMerge (root, profile->clone());
        activeProfile = name;
        return juce::Result::ok();
    }

    juce::Result Config::applyOverride (const juce::String& assignment)
    {
        auto key = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        if (key.isEmpty() || ! assignment.containsChar ('='))
            return juce::Result::fail ("Override must look like key.path=value: " + assignment);

        auto text = assignment.fromFirstOccurrenceOf ("=", false, false);
        juce::var value;
        if (juce::JSON::parse (text, value).failed() || value.isVoid())
            value = text;

        set (key, value);
        return juce::Result::ok();
    }

    juce::var Config::get (const juce::String& path) const
    {
        if (auto* v = lookup (root, path))
            return *v;
        return {};
    }

    bool Config::has (const juce::String& path) const { return lookup (root, path) != nullptr; }

    void Config::set (const juce::String& path, const juce::var& value)
    {
        auto parts = juce::StringArray::fromTokens (path, ".", {});
        if (parts.isEmpty())
            return;

        if (! root.isObject())
            root = new juce::DynamicObject();

        juce::var node = root;
        for (int i = 0; i < parts.size() - 1; ++i)
        {
            auto* obj = node.getDynamicObject();
            if (! obj->getProperty (parts[i]).isObject())
                obj->setProperty (parts[i], new juce::DynamicObject());
            node = obj->getProperty (parts[i]);
        }
        node.getDynamicObject()->setProperty (parts[parts.size() - 1], value);
    }

    juce::String Config::getString (const juce::String& path, const juce::String& fallback) const
    {
        auto v = get (path);
        return v.isVoid() ? fallback : v.toString();
    }

    int Config::getInt (const juce::String& path, int fallback) const
    {
        auto v = get (path);
        if (v.isString())
            return v.toString().startsWithIgnoreCase ("0x") ? v.toString().substring (2).getHexValue32() : v.toString().getIntValue();
        return v.isVoid() ? fallback : (int) v;
    }

    double Config::getDouble (const juce::String& path, double fallback) const
    {
        auto v = get (path);
        return v.isVoid() ? fallback : (double) v;
    }

    bool Config::getBool (const juce::String& path, bool fallback) const
    {
        auto v = get (path);
        if (v.isString())
            return v.toString().equalsIgnoreCase ("true") || v.toString() == "1";
        return v.isVoid() ? fallback : (bool) v;
    }

    juce::StringArray Config::getStringArray (const juce::String& path) const
    {
        juce::StringArray out;
        if (auto* arr = get (path).getArray())
            for (auto& item : *arr)
                out.add (item.toString());
        return out;
    }

    juce::Array<int> Config::getIntArray (const juce::String& path) const
    {
        juce::Array<int> out;
        if (auto* arr = get (path).getArray())
            for (auto& item : *arr)
                out.add ((int) item);
        return out;
    }

    juce::String Config::expandEnvironment (const juce::String& text)
    {
        juce::String out;
        int i = 0;
        while (i < text.length())
        {
            if (text[i] == '$' && text[i + 1] == '{')
            {
                auto end = text.indexOfChar (i + 2, '}');
                if (end > 0)
                {
                    auto name = text.substring (i + 2, end);
                    auto value = juce::SystemStats::getEnvironmentVariable (name, {});

                    // TMPDIR is not always exported (e.g. when launched from Finder).
                    if (value.isEmpty() && name == "TMPDIR")
                        value = juce::File::getSpecialLocation (juce::File::tempDirectory).getFullPathName();

                    out << value.trimCharactersAtEnd ("/");
                    i = end + 1;
                    continue;
                }
            }
            out << juce::String::charToString (text[i]);
            ++i;
        }

        if (out.startsWith ("~/"))
            out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName() + out.substring (1);
        return out;
    }

    juce::File Config::resolvePath (const juce::String& value) const
    {
        auto expanded = expandEnvironment (value).trim();
        if (expanded.isEmpty())
            return {};
        if (juce::File::isAbsolutePath (expanded))
            return juce::File (expanded);
        return repoRoot.getChildFile (expanded);
    }

    juce::File Config::getSocketPath (const juce::String& name) const
    {
        auto fileName = getString ("host.sockets." + name, name + ".sock");
        return getRunDir().getChildFile (fileName);
    }

    juce::StringArray Config::getProfileNames() const
    {
        juce::StringArray names;
        if (auto* obj = get ("profiles").getDynamicObject())
            for (auto& prop : obj->getProperties())
                names.add (prop.name.toString());
        return names;
    }

    juce::File Config::findRepoRoot (const juce::File& start)
    {
        for (auto dir = start; dir.exists(); dir = dir.getParentDirectory())
        {
            if (dir.getChildFile ("Emulator/config").isDirectory())
                return dir;
            if (dir.isRoot())
                break;
        }
        return {};
    }

    juce::File Config::findDefaultConfigFile()
    {
        auto rel = "Emulator/config/kickemu.json";
        auto fromExe = findRepoRoot (juce::File::getSpecialLocation (juce::File::currentExecutableFile));
        if (fromExe.isDirectory())
            return fromExe.getChildFile (rel);
        auto fromCwd = findRepoRoot (juce::File::getCurrentWorkingDirectory());
        if (fromCwd.isDirectory())
            return fromCwd.getChildFile (rel);
        return juce::File::getCurrentWorkingDirectory().getChildFile (rel);
    }

    void Config::deepMerge (juce::var& target, const juce::var& source)
    {
        auto* src = source.getDynamicObject();
        auto* dst = target.getDynamicObject();
        if (src == nullptr || dst == nullptr)
            return;

        for (auto& prop : src->getProperties())
        {
            if (prop.name.toString().startsWith ("_"))
                continue; // comments

            auto existing = dst->getProperty (prop.name);
            if (prop.value.getDynamicObject() != nullptr && existing.getDynamicObject() != nullptr)
            {
                deepMerge (existing, prop.value);
            }
            else
            {
                dst->setProperty (prop.name, prop.value);
            }
        }
    }
}
