/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/TrackAnalysis.h"

#include <functional>
#include <memory>

namespace opendj
{

/** Draws a track's waveform, either as a fixed overview of the whole thing or as
    a detail view that scrolls past a stationary playhead. */
class WaveformComponent final : public juce::Component
{
public:
    enum class Mode
    {
        overview,   ///< the whole track, with a marker showing where you are
        scrolling   ///< a window either side of the playhead, with beat markers
    };

    explicit WaveformComponent (Mode modeToUse);

    void setAnalysis (std::shared_ptr<const TrackAnalysis> newAnalysis);
    void setPosition (double seconds, double lengthSeconds);
    void setCuePoint (double seconds);

    /** How many seconds the scrolling view shows either side of the playhead. */
    void setWindowSeconds (double seconds);

    /** Called with a position in seconds when the user clicks or drags. */
    std::function<void (double)> onSeek;

    void paint (juce::Graphics& g) override;

private:
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void seekFromMouse (const juce::MouseEvent& e);

    void paintOverview (juce::Graphics& g);
    void paintScrolling (juce::Graphics& g);
    void paintEmpty (juce::Graphics& g);

    const Mode mode;
    std::shared_ptr<const TrackAnalysis> analysis;

    double positionSeconds = 0.0;
    double trackLengthSeconds = 0.0;
    double cueSeconds = 0.0;
    double windowSeconds = 3.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformComponent)
};

} // namespace opendj
