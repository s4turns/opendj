/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/TrackAnalysis.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace opendj
{

/** What a library already knows about a file before the deck decodes it. */
struct KnownTrack
{
    juce::String title;              ///< from the tags; empty means use the file name
    juce::String artist;

    bool analysed = false;           ///< true when the tempo fields are trustworthy
    double bpm = 0.0;                ///< 0 means the analyser found no stable tempo
    double firstBeatSeconds = 0.0;
    float tempoConfidence = 0.0f;
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
};

} // namespace opendj
