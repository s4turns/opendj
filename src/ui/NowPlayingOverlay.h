/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_graphics/juce_graphics.h>

#include <mutex>

namespace opendj
{

/** The track name drawn over the visuals, in the lower third, the way
    VirtualDJ shows it: it comes up when the track changes, holds for a while,
    and fades away, so the picture is clean for the rest of the song.

    `setTitle` is the message thread's side and `draw` the render thread's.
    The title is a string copy under a lock rather than a read of the deck,
    because a deck frees the track it just replaced on the message thread and
    the render thread has no business holding a pointer into it. */
class NowPlayingOverlay
{
public:
    /** Empty means nothing is playing; nothing is drawn. A changed title
        restarts the show-and-fade. */
    void setTitle (const juce::String& newTitle);

    /** How long the title is shown in full, and how long the fades take. */
    static constexpr double holdSeconds = 12.0;
    static constexpr double fadeSeconds = 1.0;

    /** 0 to 1: how visible the title is `secondsSinceChange` after it
        changed. Split out so it can be tested without drawing anything. */
    static float opacityAt (double secondsSinceChange) noexcept;

    /** Draws onto a tightly packed RGB frame, top row first. */
    void draw (unsigned char* rgb, int width, int height);

private:
    std::mutex mutex;
    juce::String title;
    double changedAtSeconds = 0.0;

    juce::Image canvas;
};

} // namespace opendj
