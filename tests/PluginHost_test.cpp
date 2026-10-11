/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "core/PluginHost.h"
#include "core/MasterEffects.h"

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

TEST_CASE ("Clearing the plugin list removes known plugins and saves the change", "[plugin][host]")
{
    ScratchFolder scratch;

    juce::KnownPluginList list;
    juce::PluginDescription effect;
    effect.name = "Room";
    effect.manufacturerName = "Acme";
    effect.pluginFormatName = "VST3";
    effect.fileOrIdentifier = "C:/nowhere/Room.vst3";
    effect.uniqueId = 1234;
    list.addType (effect);
    REQUIRE (list.createXml()->writeTo (scratch.folder.getChildFile ("plugins.xml")));

    {
        opendj::PluginHost host (scratch.folder);
        REQUIRE (host.getEffects().size() == 1);

        host.clearList();
        CHECK (host.getEffects().isEmpty());
    }

    opendj::PluginHost next (scratch.folder);
    CHECK (next.getEffects().isEmpty());
}

TEST_CASE ("Scanning a given directory does not scan default folders", "[plugin][host]")
{
    ScratchFolder scratch;
    opendj::PluginHost host (scratch.folder);

    const auto custom = scratch.folder.getChildFile ("custom_vst");
    custom.createDirectory();

    CHECK (host.scan (juce::FileSearchPath (custom.getFullPathName())) == 0);
    CHECK (host.getEffects().isEmpty());
}

TEST_CASE ("Picking a plugin instead of its folder scans the folder it is in", "[plugin][host]")
{
    ScratchFolder scratch;

    const auto vst3 = scratch.folder.getChildFile ("vst3");
    const auto bundle = vst3.getChildFile ("Room.vst3");
    const auto inner = bundle.getChildFile ("Contents").getChildFile ("x86_64-win");
    REQUIRE (inner.createDirectory());
    REQUIRE (inner.getChildFile ("Room.vst3").create());
    REQUIRE (vst3.getChildFile ("Echo.vst3").create());

    // The folder holding the plugins is taken as it is.
    CHECK (opendj::PluginHost::folderToScanFor (vst3) == vst3);

    // A bundle, anything inside one, or a single-file plugin means its folder.
    CHECK (opendj::PluginHost::folderToScanFor (bundle) == vst3);
    CHECK (opendj::PluginHost::folderToScanFor (inner) == vst3);
    CHECK (opendj::PluginHost::folderToScanFor (inner.getChildFile ("Room.vst3")) == vst3);
    CHECK (opendj::PluginHost::folderToScanFor (vst3.getChildFile ("Echo.vst3")) == vst3);
}

TEST_CASE ("Scanning dragonfly-reverb and creating instance", "[plugin][host][real]")
{
    ScratchFolder scratch;
    opendj::PluginHost host (scratch.folder);
    juce::File dragonflyFolder ("/home/myssy/work/opendj/dragonfly-reverb-3.2.10");
    if (dragonflyFolder.isDirectory())
    {
        auto count = host.scan (juce::FileSearchPath (dragonflyFolder.getFullPathName()), false);
        INFO ("Found: " << count);
        auto effects = host.getEffects();
        INFO ("Effects size: " << effects.size());
        REQUIRE (! effects.isEmpty());
        for (const auto& fx : effects)
        {
            INFO ("Effect name: " << fx.name.toStdString());
            juce::String error;
            auto instance = host.createInstance (fx, 48000.0, 512, error);
            INFO ("Error: " << error.toStdString());
            CHECK (instance != nullptr);
            if (instance != nullptr)
            {
                opendj::MasterEffects masterEffects;
                masterEffects.prepare ({ 48000.0, 512, 2 });
                CHECK (masterEffects.setPlugin (0, std::move (instance)));
                masterEffects.setType (0, opendj::MasterEffects::Type::plugin);
                masterEffects.setEnabled (0, true);
                masterEffects.setWet (0, 1.0f);
                juce::AudioBuffer<float> buffer (2, 512);
                buffer.clear();
                masterEffects.process (buffer, 512);
            }
        }
    }
}
