#include "app/AppSettings.h"

namespace kickemu
{
    AppSettings::AppSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "KickEmu";
        o.filenameSuffix = ".settings";
        o.folderName = "KickEmu";
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.millisecondsBeforeSaving = 500;
        props = std::make_unique<juce::PropertiesFile> (o);
    }

    juce::File AppSettings::getFolder() const { return props->getFile().getParentDirectory(); }

    juce::String AppSettings::getString (const juce::String& key, const juce::String& fallback) const { return props->getValue (key, fallback); }
    int AppSettings::getInt (const juce::String& key, int fallback) const { return props->getIntValue (key, fallback); }
    double AppSettings::getDouble (const juce::String& key, double fallback) const { return props->getDoubleValue (key, fallback); }
    bool AppSettings::getBool (const juce::String& key, bool fallback) const { return props->getBoolValue (key, fallback); }
    void AppSettings::set (const juce::String& key, const juce::var& value) { props->setValue (key, value); }
}
