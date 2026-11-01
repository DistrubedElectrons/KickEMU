#pragma once

#include <juce_core/juce_core.h>

namespace kickemu
{
    /**
        The emulator configuration (Emulator/config/kickemu.json plus overrides).

        Values are addressed with dotted paths such as "qemu.machine". Strings may
        contain ${VAR} references to environment variables; relative paths are
        resolved against the repository root, so the same file works from any
        working directory.
    */
    class Config
    {
    public:
        Config() = default;

        /** Loads and parses a JSON configuration file. */
        juce::Result load (const juce::File& file);

        /** Loads from a JSON string (used by tests). */
        juce::Result loadFromString (const juce::String& json, const juce::File& repoRoot);

        /** Deep-merges the object at "profiles.<name>" into the root. */
        juce::Result applyProfile (const juce::String& name);

        /** Sets a value from a "key.path=value" override. The value is parsed as
            JSON when possible (numbers, booleans, arrays), otherwise kept as a string. */
        juce::Result applyOverride (const juce::String& assignment);

        /** Raw access. Returns a void var when the path does not exist. */
        juce::var get (const juce::String& path) const;
        bool has (const juce::String& path) const;
        void set (const juce::String& path, const juce::var& value);

        juce::String getString (const juce::String& path, const juce::String& fallback = {}) const;
        int getInt (const juce::String& path, int fallback = 0) const;
        double getDouble (const juce::String& path, double fallback = 0.0) const;
        bool getBool (const juce::String& path, bool fallback = false) const;
        juce::StringArray getStringArray (const juce::String& path) const;
        juce::Array<int> getIntArray (const juce::String& path) const;

        /** Expands ${VAR} environment references. Unknown variables expand to "". */
        static juce::String expandEnvironment (const juce::String& text);

        /** Expands variables and resolves a path relative to the repository root.
            Returns an invalid File for empty values. */
        juce::File resolvePath (const juce::String& value) const;

        /** Convenience: resolvePath (getString (path)). */
        juce::File getPath (const juce::String& path) const { return resolvePath (getString (path)); }

        /** The directory holding the QEMU sockets (run_dir). */
        juce::File getRunDir() const { return getPath ("run_dir"); }

        /** Full path of a socket named in host.sockets (e.g. "mcu"). */
        juce::File getSocketPath (const juce::String& name) const;

        juce::File getFile() const { return file; }
        juce::File getRepoRoot() const { return repoRoot; }
        juce::StringArray getProfileNames() const;
        juce::String getActiveProfile() const { return activeProfile; }

        /** Finds the repository root by walking up from a start directory until a
            directory containing Emulator/config is found. */
        static juce::File findRepoRoot (const juce::File& start);

        /** The default config file: Emulator/config/kickemu.json in the repo found
            from the executable's location or the current directory. */
        static juce::File findDefaultConfigFile();

        const juce::var& getRoot() const { return root; }

    private:
        static void deepMerge (juce::var& target, const juce::var& source);

        juce::var root;
        juce::File file, repoRoot;
        juce::String activeProfile;
    };
}
