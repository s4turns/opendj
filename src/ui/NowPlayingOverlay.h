/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_graphics/juce_graphics.h>

#include "core/AudioEngine.h"

#include <mutex>

namespace opendj
{

/** What is drawn over the visuals, in the way VirtualDJ's video output does
    it: the name of what is playing, a "coming up next" line when a track is
    waiting on another deck, and the turntables of the decks in use, turning
    in the top corners.

    The titles are the message thread's (`setNowPlaying`, `setComingUp`) and
    are copied here under a lock, because a deck frees the track it replaced
    on the message thread and the render thread has no business holding a
    pointer into it. The turntables are read straight off the engine's
    atomics in `draw`, on the render thread, so they turn smoothly at the
    frame rate rather than at the message timer's. */
class NowPlayingOverlay
{
public:
    explicit NowPlayingOverlay (const AudioEngine& engineToWatch) : engine (engineToWatch) {}

    /** Empty means nothing is playing. A changed title fades in, stays for
        `overlay::holdSeconds` and fades away; the same text again does not
        bring it back. */
    void setNowPlaying (const juce::String& title);

    /** The track waiting on another deck, or empty. Shown the same way. */
    void setComingUp (const juce::String& title);

    /** Draws onto a tightly packed RGB frame, top row first. */
    void draw (unsigned char* rgb, int width, int height);

private:
    struct Line
    {
        juce::String text;
        double changedAtSeconds = 0.0;
    };

    void drawTurntable (juce::Graphics& g, juce::Point<float> centre, float radius,
                        const AudioEngine::DeckStatus& status, int deckIndex) const;

    void composite (unsigned char* rgb, int width, int top, int bottom) const;

    const AudioEngine& engine;

    std::mutex mutex;
    Line nowPlaying, comingUp;

    juce::Image canvas;
};

} // namespace opendj
