/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/TrackAnalysis.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>

namespace opendj
{

/** How many hot cue pads a deck has. */
inline constexpr int numHotCues = 8;

/** One position per pad, in seconds from the start of the track. A negative
    position is an empty pad. */
using HotCues = std::array<double, numHotCues>;

inline HotCues noHotCues() noexcept
{
    HotCues cues;
    cues.fill (-1.0);
    return cues;
}

/** A name per pad, to go with the positions. Empty means the cue is unnamed.
    Names are track metadata, not something the audio thread ever touches, so
    they travel separately from the positions a deck plays. */
using HotCueNames = std::array<juce::String, numHotCues>;

inline HotCueNames noHotCueNames()
{
    return {};
}

/** What a library already knows about a file before the deck decodes it. */
struct KnownTrack
{
    juce::String title;              ///< from the tags; empty means use the file name
    juce::String artist;

    bool analysed = false;           ///< true when the tempo fields are trustworthy
    double bpm = 0.0;                ///< 0 means the analyser found no stable tempo
    double firstBeatSeconds = 0.0;
    float tempoConfidence = 0.0f;

    HotCues hotCues = noHotCues();   ///< restored onto the pads when the track loads
};

/** Where the engine asks whether a file has been analysed before, and where it
    reports the answer when it had to do the work itself.

    An interface rather than the library class, so the engine stays free of any
    idea of a database. Both calls happen on the loader thread, never the audio
    thread, so an implementation is free to take locks and touch disk. */
class AnalysisCache
{
public:
    virtual ~AnalysisCache() = default;

    virtual std::optional<KnownTrack> lookup (const juce::File& file) = 0;

    virtual void store (const juce::File& file,
                        const TrackAnalysis& analysis,
                        double durationSeconds) = 0;

    /** Remembers a track's hot cues after one is set or cleared, so they are
        there the next time it loads. Called on a loader thread. A cache with
        nowhere to keep them can ignore it. */
    virtual void storeHotCues (const juce::File& file, const HotCues& cues)
    {
        juce::ignoreUnused (file, cues);
    }

    /** Remembers a beat grid set by hand, so it comes back the next time the
        track loads and is never replaced by the analyser's guess again.
        Called on a loader thread. A cache with nowhere to keep it can
        ignore it. */
    virtual void storeBeatGrid (const juce::File& file, double bpm, double firstBeatSeconds)
    {
        juce::ignoreUnused (file, bpm, firstBeatSeconds);
    }

    /** The names a track's hot cues were given, by slot. Called on the message
        thread when a deck needs them to label its waveform. A cache with none
        returns empty names. */
    virtual HotCueNames getHotCueNames (const juce::File& file)
    {
        juce::ignoreUnused (file);
        return noHotCueNames();
    }

    /** Names (or renames) one hot cue. The position is passed so the cue can be
        stored even if the set that created it has not reached the cache yet.
        Called on the message thread. A cache with nowhere to keep it can ignore
        it. */
    virtual void setHotCueName (const juce::File& file, int slot, double seconds, const juce::String& name)
    {
        juce::ignoreUnused (file, slot, seconds, name);
    }
};

} // namespace opendj
