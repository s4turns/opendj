/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include "core/AudioEngine.h"

#include <functional>
#include <memory>
#include <vector>

namespace opendj
{

/** Plays a list of tracks one after another on decks A and B, mixing each
    into the next with the crossfader, so a stream keeps going with nobody at
    the controls.

    It does what a DJ would: loads the next track onto the idle deck a little
    ahead of time, matches its tempo and beat to the one playing, starts it
    at the planned moment and sweeps the crossfader across over the fade
    length, then clears the old deck so it is ready for the track after.
    Tracks without a beat grid are faded without the tempo match.

    Everything here is message thread, driven by `tick` from a timer. The
    decisions are in `AutoMixMath.h`, where they are tested; this class only
    acts on them. */
class AutoMix
{
public:
    explicit AutoMix (AudioEngine& engineToDrive);
    ~AutoMix();

    //==========================================================================
    // Message thread
    //==========================================================================

    /** The playlist, in order. Replacing it while running keeps the current
        track on air and continues from the start of the new list. */
    void setQueue (std::vector<juce::File> files);
    const std::vector<juce::File>& getQueue() const noexcept { return queue; }

    /** Starts playing the queue from `index`, loading it onto a deck and
        playing it at once. From then on it keeps going until stopped. */
    void start (int index = 0);

    /** Stops managing the decks. Whatever is playing carries on. */
    void stop();

    bool isRunning() const noexcept { return phase != Phase::idle; }

    /** Begins the transition to the next track now rather than at the end. */
    void skip();

    void setFadeSeconds (double seconds) noexcept { fadeSeconds = juce::jlimit (1.0, 60.0, seconds); }
    double getFadeSeconds() const noexcept { return fadeSeconds; }

    /** At the end, go back to the top instead of stopping. On by default: a
        stream is meant to keep going. */
    void setLoop (bool shouldLoop) noexcept { loop = shouldLoop; }
    bool isLooping() const noexcept { return loop; }

    /** Index in the queue of the track on air, or -1. */
    int getCurrentIndex() const noexcept { return currentIndex; }

    /** Index of the track that comes next, or -1. */
    int getNextIndex() const noexcept;

    bool isFading() const noexcept { return phase == Phase::fading; }

    /** Called when the current track or the phase changes. */
    std::function<void()> onChanged;

    /** Advances the state machine. `nowSeconds` is any steady clock, in
        seconds; tests pass their own. */
    void tick (double nowSeconds);

private:
    enum class Phase
    {
        idle,
        starting,   ///< the first track is loading
        playing,    ///< a track is on air; watching for the moment to mix
        fading      ///< the crossfader is moving
    };

    void loadFirst (int index, int attemptsLeft);
    void loadNext (int deckIndex, int index, int attemptsLeft);
    void beginFade (double nowSeconds, int onAirDeck, int incomingDeck);
    void finishFade (int outgoingDeck);
    void changed();

    static float sideOf (int deckIndex) noexcept { return deckIndex == 0 ? -1.0f : 1.0f; }

    AudioEngine& engine;

    std::vector<juce::File> queue;
    Phase phase = Phase::idle;
    double fadeSeconds = 12.0;
    bool loop = true;

    int onAirDeck = -1;
    int currentIndex = -1;

    int pendingIndex = -1;        ///< the track loaded or loading onto the idle deck
    bool nextLoading = false;
    bool nextReady = false;
    bool skipRequested = false;

    double fadeStartSeconds = 0.0;
    double fadeLength = 0.0;
    float fadeFrom = 0.0f;
    float fadeTo = 0.0f;
    int fadeOutgoingDeck = -1;

    /** Callbacks from the loader arrive later, on the message thread, and
        must not touch an object that has gone. */
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoMix)
};

} // namespace opendj
