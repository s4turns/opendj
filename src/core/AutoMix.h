/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_core/juce_core.h>

#include "core/AudioEngine.h"
#include "core/MixPoints.h"

#include <array>
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

    In smart mode the planned moment comes from `MixPoints.h`: the outgoing
    track's outro meets the incoming track's intro, phrase on phrase, the
    fade lasts a whole number of bars, and the low EQ swaps halfway. Where
    either track has no trustworthy grid it falls back to the fixed fade.

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

    /** Mix on phrases, from the outro into the intro, rather than a fixed
        time before the end. On by default. */
    void setSmart (bool shouldBeSmart);
    bool isSmart() const noexcept { return smart; }

    /** The plan for the next mix, `smart == false` when there is none or it
        will be a plain timed fade. */
    const automix::MixPlan& getPlan() const noexcept { return plan; }

    /** At the end, go back to the top instead of stopping. On by default: a
        stream is meant to keep going. */
    void setLoop (bool shouldLoop) noexcept { loop = shouldLoop; }
    bool isLooping() const noexcept { return loop; }

    /** Turn on key lock for a deck whose tempo is changed to match, so it
        stays in its own key. On by default. A deck automix turned it on for
        gets the user's setting back when automix stops, unless it is still
        playing, where the change would be heard as a jump in pitch. */
    void setKeyLock (bool shouldLock) noexcept { keyLock = shouldLock; }
    bool isKeyLocking() const noexcept { return keyLock; }

    /** Keep two vocals from being sung over each other. Each track is
        separated while the one before it plays, where a separator is
        installed; the mix is planned around the vocals and, where they still
        meet, the outgoing ones make way. On by default; without stems the
        mix is planned as before. */
    void setVocalAware (bool shouldAvoid);
    bool isVocalAware() const noexcept { return vocalAware; }

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
    void planNextMix();
    void restoreLowEq();
    void restoreVocals();
    void requestStems (int deckIndex);
    std::vector<bool> vocalsOn (int deckIndex) const;
    void changed();

    /** The most a track is stretched to match the one it is mixed into. */
    static constexpr double maxSyncStretch = 0.12;

    static float sideOf (int deckIndex) noexcept { return deckIndex == 0 ? -1.0f : 1.0f; }

    AudioEngine& engine;

    std::vector<juce::File> queue;
    Phase phase = Phase::idle;
    double fadeSeconds = 12.0;
    bool loop = true;
    bool smart = true;
    bool keyLock = true;
    bool vocalAware = true;
    automix::MixPlan plan;

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
    int fadeIncomingDeck = -1;

    /** The low EQ the user had on each side, while a smart fade swaps them. */
    bool swappingLows = false;
    float outgoingLow = 0.5f, incomingLow = 0.5f;

    /** The outgoing vocals' gain the user had, while a fade hands them over. */
    int vocalHandoffBar = -1;
    double fadeBars = 0.0;
    float outgoingVocals = 1.0f;

    /** Key lock as each of decks A and B had it before automix turned it on;
        -1 where automix has not touched it. */
    std::array<int, 2> keyLockBefore { -1, -1 };

    /** Callbacks from the loader arrive later, on the message thread, and
        must not touch an object that has gone. */
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoMix)
};

} // namespace opendj
