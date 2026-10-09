/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/PluginHost.h"

namespace opendj
{

PluginHost::PluginHost (const juce::File& folderToUse)
    : folder (folderToUse)
{
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    if (auto xml = juce::XmlDocument::parse (listFile()))
        knownPlugins.recreateFromXml (*xml);
}

PluginHost::~PluginHost()
{
    stopScan = true;

    if (scanThread.joinable())
        scanThread.join();
}

juce::File PluginHost::defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("OpenDJ");
}

juce::Array<juce::PluginDescription> PluginHost::getEffects() const
{
    juce::Array<juce::PluginDescription> effects;

    for (const auto& description : knownPlugins.getTypes())
        if (! description.isInstrument)
            effects.add (description);

    std::sort (effects.begin(), effects.end(), [] (const auto& a, const auto& b)
    {
        const auto maker = a.manufacturerName.compareIgnoreCase (b.manufacturerName);
        return maker != 0 ? maker < 0 : a.name.compareIgnoreCase (b.name) < 0;
    });

    return effects;
}

void PluginHost::saveList() const
{
    folder.createDirectory();

    if (auto xml = knownPlugins.createXml())
        xml->writeTo (listFile());
}

juce::File PluginHost::folderToScanFor (const juce::File& picked)
{
    // The outermost .vst3 wins: on Windows a bundle holds another .vst3 file
    // in Contents/x86_64-win, and its folder would hold only that one plugin.
    juce::File plugin;

    for (auto f = picked; f != f.getParentDirectory(); f = f.getParentDirectory())
        if (f.hasFileExtension (".vst3"))
            plugin = f;

    if (plugin != juce::File())
        return plugin.getParentDirectory();

    return picked.isDirectory() ? picked : picked.getParentDirectory();
}

void PluginHost::clearList()
{
    knownPlugins.clear();
    saveList();
}

int PluginHost::scan (const juce::FileSearchPath& folders, bool includeDefaultFolders,
                      const std::atomic<bool>* shouldStop)
{
    folder.createDirectory();

    auto* format = formats.getFormat (0);
    auto path = folders;

    if (includeDefaultFolders)
        path.addPath (format->getDefaultLocationsToSearch());

    path.removeRedundantPaths();
    path.removeNonExistentPaths();

    const auto before = knownPlugins.getNumTypes();

    // The pedal file names the plugin being tried. If it kills the process,
    // the next scan finds the name still there and leaves that file out.
    juce::PluginDirectoryScanner scanner (knownPlugins, *format, path, true, pedalFile());
    juce::String name;

    while (! (shouldStop != nullptr && shouldStop->load()) && scanner.scanNextFile (true, name))
    {
    }

    saveList();
    return knownPlugins.getNumTypes() - before;
}

void PluginHost::scanAsync (const juce::FileSearchPath& folders,
                            std::function<void (int, juce::StringArray)> onDone,
                            bool includeDefaultFolders)
{
    if (scanning.exchange (true))
        return;

    if (scanThread.joinable())
        scanThread.join();

    stopScan = false;

    scanThread = std::thread ([this, folders, includeDefaultFolders, onDone = std::move (onDone)]
    {
        scan (folders, includeDefaultFolders, &stopScan);

        // A file that crashed a scan stays in the pedal file's blacklist; the
        // rest of the failures are worth telling somebody about.
        const auto failed = knownPlugins.getBlacklistedFiles();
        const auto count = getEffects().size();
        scanning = false;

        juce::MessageManager::callAsync ([onDone, count, failed]
        {
            if (onDone != nullptr)
                onDone (count, failed);
        });
    });
}

void PluginHost::scanAsync (std::function<void (int, juce::StringArray)> onDone)
{
    scanAsync ({}, std::move (onDone), false);
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                        double sampleRate, int blockSize,
                                                                        juce::String& error)
{
    auto instance = formats.createPluginInstance (description, sampleRate, blockSize, error);

    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = description.name + " could not be loaded";

        return nullptr;
    }

    // A master effect is two channels in and two out. Side chains and the
    // like are switched off rather than fed with nothing.
    instance->disableNonMainBuses();

    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());

    if (! instance->setBusesLayout (stereo))
    {
        error = description.name + " is not a stereo effect";
        return nullptr;
    }

    return instance;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::String& identifier,
                                                                        double sampleRate, int blockSize,
                                                                        juce::String& error)
{
    if (auto description = knownPlugins.getTypeForIdentifierString (identifier))
        return createInstance (*description, sampleRate, blockSize, error);

    error = "a plugin that is no longer installed";
    return nullptr;
}

void PluginHost::beginRiskyLoad (const juce::String& pluginName)
{
    folder.createDirectory();
    loadingFile().replaceWithText (pluginName);
}

void PluginHost::endRiskyLoad()
{
    loadingFile().deleteFile();
}

juce::String PluginHost::takeCrashedPluginName()
{
    const auto file = loadingFile();

    if (! file.existsAsFile())
        return {};

    auto name = file.loadFileAsString().trim();
    file.deleteFile();
    return name.isNotEmpty() ? name : juce::String ("a plugin");
}

} // namespace opendj
