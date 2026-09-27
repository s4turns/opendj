/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "core/PluginHost.h"

namespace
{
    /** A folder of its own, taken away again afterwards. */
    struct ScratchFolder
    {
        ScratchFolder()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("opendj-plugins-" + juce::Uuid().toString()))
        {
            folder.createDirectory();
        }

        ~ScratchFolder() { folder.deleteRecursively(); }

        juce::File folder;
    };
}

TEST_CASE ("Scanning a folder with no plugins in it finds none", "[plugin][host]")
{
    ScratchFolder scratch;
    opendj::PluginHost host (scratch.folder);

    const auto empty = scratch.folder.getChildFile ("empty");
    empty.createDirectory();

    CHECK (host.scan (juce::FileSearchPath (empty.getFullPathName()), false) == 0);
    CHECK (host.getEffects().isEmpty());

    // The list is written out even when it is empty, so the next start has
    // one to read.
    CHECK (scratch.folder.getChildFile ("plugins.xml").existsAsFile());
}

TEST_CASE ("The plugin list is read back when the host starts again", "[plugin][host]")
{
    ScratchFolder scratch;

    // A list as a previous run would have left it, naming one effect and one
    // instrument. Only the effect belongs in a master slot.
    juce::KnownPluginList list;

    juce::PluginDescription effect;
    effect.name = "Room";
    effect.manufacturerName = "Acme";
    effect.pluginFormatName = "VST3";
    effect.fileOrIdentifier = "C:/nowhere/Room.vst3";
    effect.uniqueId = 1234;
    effect.numInputChannels = 2;
    effect.numOutputChannels = 2;

    auto instrument = effect;
    instrument.name = "Synth";
    instrument.fileOrIdentifier = "C:/nowhere/Synth.vst3";
    instrument.uniqueId = 5678;
    instrument.isInstrument = true;

    list.addType (effect);
    list.addType (instrument);
    REQUIRE (list.createXml()->writeTo (scratch.folder.getChildFile ("plugins.xml")));

    opendj::PluginHost host (scratch.folder);
    const auto effects = host.getEffects();

    REQUIRE (effects.size() == 1);
    CHECK (effects[0].name == "Room");

    // A saved slot names its plugin by identifier; one that is not installed
    // any more fails with a reason rather than crashing.
    juce::String error;
    CHECK (host.createInstance (effect.createIdentifierString(), 48000.0, 512, error) == nullptr);
    CHECK (error.isNotEmpty());

    error = {};
    CHECK (host.createInstance ("VST3-Nothing-0-0", 48000.0, 512, error) == nullptr);
    CHECK (error.contains ("no longer installed"));
}

TEST_CASE ("A plugin that killed the last start-up is named once", "[plugin][host]")
{
    ScratchFolder scratch;

    {
        opendj::PluginHost host (scratch.folder);

        // Loading completes: nothing is left behind.
        host.beginRiskyLoad ("Fine Verb");
        host.endRiskyLoad();
        CHECK (host.takeCrashedPluginName().isEmpty());

        // Loading never completes, as if the process had died in the middle.
        host.beginRiskyLoad ("Bad Delay");
    }

    opendj::PluginHost next (scratch.folder);
    CHECK (next.takeCrashedPluginName() == "Bad Delay");
    CHECK (next.takeCrashedPluginName().isEmpty());
}
