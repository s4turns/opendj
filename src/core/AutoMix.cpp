/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/AutoMix.h"

#include "core/AutoMixMath.h"

namespace opendj
{

AutoMix::AutoMix (AudioEngine& engineToDrive) : engine (engineToDrive) {}

AutoMix::~AutoMix()
{
    *alive = false;
}

void AutoMix::changed()
{
    if (onChanged != nullptr)
        onChanged();
}

void AutoMix::setQueue (std::vector<juce::File> files)
{
    queue = std::move (files);

    // What was planned to come next came from the old list.
    if (phase == Phase::playing && ! nextLoading && ! nextReady)
        pendingIndex = -1;

    changed();
}

int AutoMix::getNextIndex() const noexcept
{
    if (pendingIndex >= 0)
        return pendingIndex;

    return automix::nextIndex (currentIndex, (int) queue.size(), loop);
}

void AutoMix::start (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) queue.size()))
        return;

    // Decks A and B answer to the crossfader; C and D are left as they were.
    auto& mixer = engine.getMixer();
    mixer.setChannelCrossfaderAssign (0, Mixer::CrossfaderAssign::a);
    mixer.setChannelCrossfaderAssign (1, Mixer::CrossfaderAssign::b);

    // Whatever was on these decks is replaced.
    for (int i = 0; i < 2; ++i)
        if (! engine.isDeckLoading (i))
            engine.getDeck (i).pause();

    phase = Phase::starting;
    nextLoading = nextReady = skipRequested = false;
    pendingIndex = -1;
    currentIndex = -1;
    onAirDeck = -1;

    loadFirst (index, (int) queue.size());
    changed();
}

void AutoMix::stop()
{
    phase = Phase::idle;
    nextLoading = nextReady = skipRequested = false;
    pendingIndex = -1;
    currentIndex = -1;
    onAirDeck = -1;
    changed();
}

void AutoMix::skip()
{
    if (phase == Phase::playing)
        skipRequested = true;
}

void AutoMix::loadFirst (int index, int attemptsLeft)
{
    if (attemptsLeft <= 0 || ! juce::isPositiveAndBelow (index, (int) queue.size()))
    {
        stop();
        return;
    }

    // Deck A unless the user has it loading something; B otherwise.
    const auto deckIndex = engine.isDeckLoading (0) ? 1 : 0;
    const auto file = queue[(size_t) index];
    const auto token = alive;

    engine.loadTrackAsync (deckIndex, file, [this, token, deckIndex, index, attemptsLeft] (bool ok)
    {
        if (! *token || phase != Phase::starting)
            return;

        if (! ok)
        {
            loadFirst (automix::nextIndex (index, (int) queue.size(), loop), attemptsLeft - 1);
            return;
        }

        engine.getMixer().setCrossfaderPosition (sideOf (deckIndex));
        engine.getDeck (deckIndex).play();

        onAirDeck = deckIndex;
        currentIndex = index;
        phase = Phase::playing;
        changed();
    });
}

void AutoMix::loadNext (int deckIndex, int index, int attemptsLeft)
{
    if (attemptsLeft <= 0 || ! juce::isPositiveAndBelow (index, (int) queue.size()))
    {
        nextLoading = false;
        pendingIndex = -1;
        return;
    }

    nextLoading = true;
    pendingIndex = index;
    const auto file = queue[(size_t) index];
    const auto token = alive;

    engine.loadTrackAsync (deckIndex, file, [this, token, deckIndex, index, attemptsLeft] (bool ok)
    {
        if (! *token || phase == Phase::idle)
            return;

        if (! ok)
        {
            // An unreadable or missing file is skipped, not allowed to stall
            // the stream.
            const auto after = automix::nextIndex (index, (int) queue.size(), loop);
            loadNext (deckIndex, after, attemptsLeft - 1);
            changed();
            return;
        }

        nextLoading = false;
        nextReady = true;
        pendingIndex = index;
        changed();
    });
}

void AutoMix::beginFade (double nowSeconds, int outgoing, int incoming)
{
    // Tempo and beat matched to what is playing, where both have a grid; a
    // track without one just fades in at its own speed.
    engine.syncDeck (incoming, outgoing);
    engine.getDeck (incoming).play();

    const auto& out = engine.getDeck (outgoing);
    const auto& in = engine.getDeck (incoming);

    fadeLength = automix::effectiveFadeSeconds (fadeSeconds, out.getLengthSeconds(), in.getLengthSeconds());
    fadeStartSeconds = nowSeconds;
    fadeFrom = engine.getMixer().getCrossfaderPosition();
    fadeTo = sideOf (incoming);
    fadeOutgoingDeck = outgoing;
    phase = Phase::fading;

    currentIndex = pendingIndex;
    pendingIndex = -1;
    nextReady = false;
    skipRequested = false;
    onAirDeck = incoming;
    changed();
}

void AutoMix::finishFade (int outgoing)
{
    engine.getMixer().setCrossfaderPosition (fadeTo);

    // The old track is done. Clearing the deck matters beyond tidiness: a
    // loaded, stopped deck is what the visuals announce as coming up next.
    engine.getDeck (outgoing).pause();
    engine.getDeck (outgoing).unload();

    phase = Phase::playing;
    changed();
}

void AutoMix::tick (double nowSeconds)
{
    if (phase == Phase::idle || phase == Phase::starting)
        return;

    if (phase == Phase::fading)
    {
        const auto progress = fadeLength > 0.0 ? (nowSeconds - fadeStartSeconds) / fadeLength : 1.0;
        engine.getMixer().setCrossfaderPosition (automix::fadePosition (progress, fadeFrom, fadeTo));

        if (progress >= 1.0)
            finishFade (fadeOutgoingDeck);

        return;
    }

    // Playing. A deck the user paused or ran off the end of stops being on
    // air: carry on only if there is something to carry on to.
    const auto& onAir = engine.getDeck (onAirDeck);
    const auto incomingDeck = 1 - onAirDeck;

    if (! onAir.isPlaying())
    {
        // Ran to the end with nothing mixed in: go straight to the next.
        const auto atEnd = onAir.getLengthSeconds() > 0.0
                        && onAir.getPositionSeconds() >= onAir.getLengthSeconds() - 0.05;

        if (atEnd && ! nextLoading)
        {
            const auto next = automix::nextIndex (currentIndex, (int) queue.size(), loop);

            if (next < 0)
            {
                stop();
                return;
            }

            start (next);
        }

        return;
    }

    const auto speed = juce::jmax (0.05, onAir.getTempoRatio());

    automix::Situation situation;
    situation.remainingSeconds = (onAir.getLengthSeconds() - onAir.getPositionSeconds()) / speed;
    situation.haveNext = getNextIndex() >= 0;
    situation.nextReady = nextReady;
    situation.nextLoading = nextLoading;
    situation.skipRequested = skipRequested;

    switch (automix::decide (situation, fadeSeconds))
    {
        case automix::Step::load:
            if (! engine.isDeckLoading (incomingDeck))
                loadNext (incomingDeck, getNextIndex(), (int) queue.size());
            break;

        case automix::Step::fade:
            beginFade (nowSeconds, onAirDeck, incomingDeck);
            break;

        case automix::Step::wait:
            break;
    }
}

} // namespace opendj
