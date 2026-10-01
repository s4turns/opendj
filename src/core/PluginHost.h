/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace opendj
{

/** Finds VST3 effects on the machine and makes instances of them for the
    master effect slots.

    Everything here runs on the message thread except the scan itself, which
    runs on a thread of its own and reports back to the message thread.

    Plugins run inside OpenDJ, so one that crashes takes the application with
    it. Two guards keep that from becoming a loop: the scan keeps a dead man's
    pedal file, so a plugin that kills a scan is left out of the next one; and
    restoring a plugin from the settings leaves a marker behind until it has
    loaded, so a plugin that kills the start-up is named and skipped the next
    time rather than loaded again.
*/
class PluginHost
{
public:
    /** Keeps its list and its guard files in the given folder. */
    explicit PluginHost (const juce::File& folderToUse = defaultFolder());
    ~PluginHost();

    /** Beside the settings: `%APPDATA%\OpenDJ` on Windows, `~/.config/OpenDJ` on Linux. */
    static juce::File defaultFolder();

    /** Every effect found so far, instruments left out, sorted by maker and name. */
    juce::Array<juce::PluginDescription> getEffects() const;

    /** Looks for plugins in the given folders, and in the usual VST3 folders
        when told to. Blocks until done or until shouldStop is set; adds what it
        finds to the list and saves the list. Returns the number found. */
    int scan (const juce::FileSearchPath& folders, bool includeDefaultFolders,
              const std::atomic<bool>* shouldStop = nullptr);

    /** The same on a thread of its own, reporting to the message thread with
        how many effects are known and which files failed. Ignored while a scan
        is already running. */
    void scanAsync (std::function<void (int effectsKnown, juce::StringArray failedFiles)> onDone);
    bool isScanning() const noexcept { return scanning.load(); }

    /** A plugin, set up as a stereo effect, or null with a reason why. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& description,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

    /** The same for a plugin named by `PluginDescription::createIdentifierString`,
        as the settings keep it. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::String& identifier,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error);

    //==========================================================================
    // The start-up guard

    /** Call before loading a plugin that nobody has just chosen, such as one
        restored from the settings, and endRiskyLoad once it is loaded. */
    void beginRiskyLoad (const juce::String& pluginName);
    void endRiskyLoad();

    /** The plugin the last run died loading, or empty. Clears the marker, so
        it is reported once. */
    juce::String takeCrashedPluginName();

private:
    juce::File listFile() const        { return folder.getChildFile ("plugins.xml"); }
    juce::File pedalFile() const       { return folder.getChildFile ("plugin-scan.txt"); }
    juce::File loadingFile() const     { return folder.getChildFile ("plugin-loading.txt"); }

    void saveList() const;

    juce::File folder;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;

    std::atomic<bool> scanning { false }, stopScan { false };
    std::thread scanThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};

} // namespace opendj
