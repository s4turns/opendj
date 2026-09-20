/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/Settings.h"

using Catch::Matchers::WithinAbs;
using opendj::MicInput;
using opendj::Mixer;
using opendj::Sampler;
using opendj::SessionState;

namespace
{
    /** A state with nothing left at its default, so a field the round trip
        drops shows up as a failure rather than passing by coincidence. */
    SessionState populated()
    {
        SessionState state;

        state.masterGain = 0.63f;
        state.cueGain = 0.41f;
        state.cueMix = 0.75f;
        state.crossfaderCurve = Mixer::CrossfaderCurve::sharpCut;

        state.crossfaderAssigns = { Mixer::CrossfaderAssign::thru,
                                    Mixer::CrossfaderAssign::a,
                                    Mixer::CrossfaderAssign::b,
                                    Mixer::CrossfaderAssign::a };

        state.tempoRanges = { 16.0, 50.0, 6.0, 100.0 };
        state.visibleDecks = { 2, 3 };
        state.waveformZoomSeconds = 8.0;

        state.samplerGain = 0.55f;
        state.samplerCue = true;
        state.windowBounds = "100 120 1600 900";

        state.micGain = 1.4f;
        state.micTalkover = true;
        state.micRouting = MicInput::Routing::recordingOnly;

        state.rtmp.server = "rtmp://live.twitch.tv/app";
        state.rtmp.streamKey = "live_123_abc";
        state.rtmp.streamTitle = "Friday set";
        state.rtmp.videoWidth = 1920;
        state.rtmp.videoHeight = 1080;
        state.rtmp.videoBitrateKbps = 4500;
        state.rtmp.audioBitrateKbps = 128;
        state.rtmp.fps = 25;

        for (int slot = 0; slot < Sampler::numSlots; ++slot)
        {
            state.samplerFiles[(size_t) slot] = "/music/stab" + juce::String (slot) + ".wav";
            state.samplerLooping[(size_t) slot] = slot % 2 == 0;
            state.samplerGains[(size_t) slot] = 0.2f + 0.1f * (float) slot;
        }

        return state;
    }

    void requireSame (const SessionState& a, const SessionState& b)
    {
        REQUIRE_THAT (a.masterGain, WithinAbs (b.masterGain, 1.0e-4f));
        REQUIRE_THAT (a.cueGain, WithinAbs (b.cueGain, 1.0e-4f));
        REQUIRE_THAT (a.cueMix, WithinAbs (b.cueMix, 1.0e-4f));
        REQUIRE (a.crossfaderCurve == b.crossfaderCurve);
        REQUIRE (a.crossfaderAssigns == b.crossfaderAssigns);
        REQUIRE (a.tempoRanges == b.tempoRanges);
        REQUIRE (a.visibleDecks == b.visibleDecks);
        REQUIRE_THAT (a.waveformZoomSeconds, WithinAbs (b.waveformZoomSeconds, 1.0e-9));
        REQUIRE_THAT (a.samplerGain, WithinAbs (b.samplerGain, 1.0e-4f));
        REQUIRE (a.samplerCue == b.samplerCue);
        REQUIRE (a.samplerFiles == b.samplerFiles);
        REQUIRE (a.samplerLooping == b.samplerLooping);
        REQUIRE (a.windowBounds == b.windowBounds);
        REQUIRE_THAT (a.micGain, WithinAbs (b.micGain, 1.0e-4f));
        REQUIRE (a.micTalkover == b.micTalkover);
        REQUIRE (a.micRouting == b.micRouting);

        REQUIRE (a.rtmp.server == b.rtmp.server);
        REQUIRE (a.rtmp.streamKey == b.rtmp.streamKey);
        REQUIRE (a.rtmp.streamTitle == b.rtmp.streamTitle);
        REQUIRE (a.rtmp.videoWidth == b.rtmp.videoWidth);
        REQUIRE (a.rtmp.videoHeight == b.rtmp.videoHeight);
        REQUIRE (a.rtmp.videoBitrateKbps == b.rtmp.videoBitrateKbps);
        REQUIRE (a.rtmp.audioBitrateKbps == b.rtmp.audioBitrateKbps);
        REQUIRE (a.rtmp.fps == b.rtmp.fps);

        // Never saved: it is decided at the moment of going live from
        // whether the visuals are running, and a stale yes would put a
        // black picture on a stream started with them off.
        REQUIRE_FALSE (a.rtmp.liveVideo);

        for (size_t slot = 0; slot < Sampler::numSlots; ++slot)
            REQUIRE_THAT (a.samplerGains[slot], WithinAbs (b.samplerGains[slot], 1.0e-4f));
    }

    /** A scratch file that takes itself away again. */
    struct TemporaryFile
    {
        TemporaryFile()
            : file (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("opendj-settings-" + juce::Uuid().toString() + ".json"))
        {}

        ~TemporaryFile() { file.deleteFile(); }

        juce::File file;
    };
}

TEST_CASE ("settings survive a round trip through JSON", "[settings]")
{
    const auto original = populated();
    requireSame (SessionState::fromVar (original.toVar()), original);
}

TEST_CASE ("settings survive a round trip through a file", "[settings]")
{
    TemporaryFile temporary;
    const auto original = populated();

    REQUIRE (original.writeTo (temporary.file));
    REQUIRE (temporary.file.existsAsFile());

    requireSame (SessionState::readFrom (temporary.file), original);

    // Written as JSON rather than something only this program can read, so a
    // setting can be corrected by hand when the interface for it does not exist.
    const auto text = temporary.file.loadFileAsString();
    REQUIRE (text.contains ("master_gain"));
    REQUIRE (text.contains ("sharp_cut"));
}

TEST_CASE ("a missing settings file is the defaults, not an error", "[settings]")
{
    const juce::File nowhere ("/no/such/opendj/settings.json");

    requireSame (SessionState::readFrom (nowhere), SessionState());
}

TEST_CASE ("a damaged settings file falls back field by field", "[settings]")
{
    // Half of this is nonsense and half is usable, which is what a hand edited
    // file looks like. The usable half must survive.
    const auto damaged = juce::JSON::parse (R"({
        "master_gain": 0.5,
        "cue_gain": "loud",
        "crossfader_curve": "not a curve",
        "tempo_ranges": [16.0, "eight"],
        "visible_decks": [2, 2],
        "sampler_gains": [0.25],
        "sampler_looping": true
    })");

    const auto state = SessionState::fromVar (damaged);
    const SessionState defaults;

    REQUIRE_THAT (state.masterGain, WithinAbs (0.5f, 1.0e-4f));
    REQUIRE_THAT (state.cueGain, WithinAbs (defaults.cueGain, 1.0e-4f));
    REQUIRE (state.crossfaderCurve == Mixer::CrossfaderCurve::constantPower);

    REQUIRE_THAT (state.tempoRanges[0], WithinAbs (16.0, 1.0e-9));
    REQUIRE_THAT (state.tempoRanges[1], WithinAbs (defaults.tempoRanges[1], 1.0e-9));

    // The right hand side can only show B or D, so a file asking for C there is
    // ignored rather than obeyed into a window with the same deck twice.
    REQUIRE (state.visibleDecks[0] == 2);
    REQUIRE (state.visibleDecks[1] == defaults.visibleDecks[1]);

    REQUIRE_THAT (state.samplerGains[0], WithinAbs (0.25f, 1.0e-4f));
    REQUIRE_THAT (state.samplerGains[1], WithinAbs (defaults.samplerGains[1], 1.0e-4f));
    REQUIRE (state.samplerLooping == defaults.samplerLooping);
}

TEST_CASE ("out of range values are brought back in range", "[settings]")
{
    const auto absurd = juce::JSON::parse (R"({
        "master_gain": 40.0,
        "cue_mix": -3.0,
        "tempo_ranges": [1000.0, 0.0, 8.0, 8.0],
        "sampler_gains": [99.0, -5.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
        "mic_gain": 9.0
    })");

    const auto state = SessionState::fromVar (absurd);

    REQUIRE (state.masterGain <= 1.0f);
    REQUIRE (state.cueMix >= 0.0f);
    REQUIRE (state.tempoRanges[0] <= 100.0);
    REQUIRE (state.tempoRanges[1] >= 1.0);
    REQUIRE (state.samplerGains[0] <= 2.0f);
    REQUIRE (state.samplerGains[1] >= 0.0f);
    REQUIRE (state.micGain <= 2.0f);
}

TEST_CASE ("a waveform zoom is snapped to a rung of the ladder", "[settings][waveform]")
{
    // Nobody types a zoom into the file, but somebody reading it might try, and
    // the nearest view that exists is a better answer than the default.
    const auto edited = juce::JSON::parse (R"({ "waveform_zoom_seconds": 3.4 })");
    REQUIRE_THAT (SessionState::fromVar (edited).waveformZoomSeconds, WithinAbs (3.0, 1.0e-9));

    const auto absurd = juce::JSON::parse (R"({ "waveform_zoom_seconds": 900.0 })");
    REQUIRE_THAT (SessionState::fromVar (absurd).waveformZoomSeconds,
                  WithinAbs (opendj::waveform::zoomLevels.back(), 1.0e-9));

    const auto nonsense = juce::JSON::parse (R"({ "waveform_zoom_seconds": "wide" })");
    REQUIRE_THAT (SessionState::fromVar (nonsense).waveformZoomSeconds,
                  WithinAbs (SessionState().waveformZoomSeconds, 1.0e-9));
}

TEST_CASE ("a state reaches the mixer and the sampler it was saved from", "[settings]")
{
    Mixer mixer;
    Sampler sampler;

    auto state = populated();
    state.applyTo (mixer, sampler);

    REQUIRE_THAT (mixer.getMasterGain(), WithinAbs (state.masterGain, 1.0e-3f));
    REQUIRE_THAT (mixer.getCueGain(), WithinAbs (state.cueGain, 1.0e-3f));
    REQUIRE_THAT (mixer.getCueMix(), WithinAbs (state.cueMix, 1.0e-3f));
    REQUIRE (mixer.getCrossfaderCurve() == Mixer::CrossfaderCurve::sharpCut);

    for (int c = 0; c < Mixer::numChannels; ++c)
        REQUIRE (mixer.getChannelCrossfaderAssign (c) == state.crossfaderAssigns[(size_t) c]);

    REQUIRE_THAT (sampler.getGain(), WithinAbs (state.samplerGain, 1.0e-3f));
    REQUIRE (sampler.isCueEnabled() == state.samplerCue);

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        REQUIRE (sampler.isSlotLooping (slot) == state.samplerLooping[(size_t) slot]);
        REQUIRE_THAT (sampler.getSlotGain (slot),
                      WithinAbs (state.samplerGains[(size_t) slot], 1.0e-3f));
    }
}

TEST_CASE ("what a mixer and sampler hold is what gets captured", "[settings]")
{
    Mixer mixer;
    Sampler sampler;

    mixer.setMasterGain (0.42f);
    mixer.setCrossfaderCurve (Mixer::CrossfaderCurve::linear);
    mixer.setChannelCrossfaderAssign (2, Mixer::CrossfaderAssign::b);
    sampler.setGain (0.31f);
    sampler.setCueEnabled (true);
    sampler.setSlotLooping (5, true);

    SessionState state;
    state.captureFrom (mixer, sampler);

    REQUIRE_THAT (state.masterGain, WithinAbs (0.42f, 1.0e-3f));
    REQUIRE (state.crossfaderCurve == Mixer::CrossfaderCurve::linear);
    REQUIRE (state.crossfaderAssigns[2] == Mixer::CrossfaderAssign::b);
    REQUIRE_THAT (state.samplerGain, WithinAbs (0.31f, 1.0e-3f));
    REQUIRE (state.samplerCue);
    REQUIRE (state.samplerLooping[5]);

    // An empty pad names no file, rather than naming one that is not there.
    REQUIRE (state.samplerFiles[0].isEmpty());
}

TEST_CASE ("the mic's settings are kept, but never whether it was on", "[settings][mic]")
{
    MicInput mic;
    mic.setGain (1.5f);
    mic.setTalkover (true);
    mic.setRouting (MicInput::Routing::recordingOnly);
    mic.setEnabled (true);

    SessionState state;
    state.captureFrom (mic);

    // A mic left on when the application closed must not come back on.
    MicInput restored;
    restored.setEnabled (true);
    SessionState::fromVar (state.toVar()).applyTo (restored);

    REQUIRE_THAT (restored.getGain(), WithinAbs (1.5f, 1.0e-3f));
    REQUIRE (restored.isTalkoverEnabled());
    REQUIRE (restored.getRouting() == MicInput::Routing::recordingOnly);
    REQUIRE (! restored.isEnabled());
}

TEST_CASE ("saving over settings that already exist replaces them", "[settings]")
{
    TemporaryFile temporary;

    SessionState first;
    first.masterGain = 0.9f;
    REQUIRE (first.writeTo (temporary.file));

    SessionState second;
    second.masterGain = 0.1f;
    REQUIRE (second.writeTo (temporary.file));

    REQUIRE_THAT (SessionState::readFrom (temporary.file).masterGain,
                  WithinAbs (0.1f, 1.0e-4f));

    // The file the write went through first is not left lying beside it.
    REQUIRE (! temporary.file.getSiblingFile (temporary.file.getFileName() + ".tmp").exists());
}
