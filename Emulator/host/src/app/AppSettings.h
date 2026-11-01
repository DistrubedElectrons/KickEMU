#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace kickemu
{
    /**
        Per-user settings: window layout and UI choices, persisted in
        ~/Library/Application Support/KickEmu/KickEmu.settings.
        Emulator configuration lives in the JSON config, not here.
    */
    class AppSettings
    {
    public:
        AppSettings();

        juce::PropertiesFile& file() { return *props; }

        juce::String getString (const juce::String& key, const juce::String& fallback = {}) const;
        int getInt (const juce::String& key, int fallback) const;
        double getDouble (const juce::String& key, double fallback) const;
        bool getBool (const juce::String& key, bool fallback) const;
        void set (const juce::String& key, const juce::var& value);

        juce::File getFolder() const;

    private:
        std::unique_ptr<juce::PropertiesFile> props;
    };
}
