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

void AutoMix::setSmart (bool shouldBeSmart)
{
    smart = shouldBeSmart;
    planNextMix();
    changed();
}

void AutoMix::setVocalAware (bool shouldAvoid)
{
    vocalAware = shouldAvoid;

    if (vocalAware && phase == Phase::playing && onAirDeck >= 0)
    {
        requestStems (onAirDeck);

        if (nextReady)
            requestStems (1 - onAirDeck);
    }

    planNextMix();
    changed();
}

std::vector<bool> AutoMix::vocalsOn (int deckIndex) const
{
    const auto& deck = engine.getDeck (deckIndex);
    const auto stems = deck.getSeparation();
    const auto analysis = deck.getAnalysis();

    if (stems == nullptr || analysis == nullptr)
        return {};

    return automix::vocalBars (*stems, *analysis);
}

void AutoMix::requestStems (int deckIndex)
{
    if (! vocalAware || engine.getDeck (deckIndex).hasStems() || ! engine.getStemSeparator().isAvailable())
        return;

    // Minutes of work at most, and the track before has minutes to play, so
    // the vocals are known by the time the mix comes round. The plan is made
    // again when they are.
    const auto token = alive;

    engine.separateDeckAsync (deckIndex, [this, token] (bool ok)
    {
        if (! *token || ! ok || phase != Phase::playing)
            return;

        planNextMix();
        changed();
    });
}

void AutoMix::planNextMix()
{
    plan = {};

    if (onAirDeck >= 0)
    {
        plannedOutgoing = engine.getDeck (onAirDeck).getAnalysis();
        plannedIncoming = engine.getDeck (1 - onAirDeck).getAnalysis();
    }

    if (! smart || phase != Phase::playing || ! nextReady || onAirDeck < 0)
        return;

    const auto& onAir = engine.getDeck (onAirDeck);
    const auto outgoing = onAir.getAnalysis();
    const auto incoming = engine.getDeck (1 - onAirDeck).getAnalysis();

    if (outgoing == nullptr || incoming == nullptr)
        return;

    // Already past the phrase it wanted, which happens when the next track
    // took long to load or the user moved on: a timed fade it is.
    const auto stillAhead = [&] (const automix::MixPlan& candidate)
    {
        return candidate.smart && onAir.getPositionSeconds() <= candidate.outSeconds;
    };

    plan = automix::planMix (*outgoing, *incoming);

    if (! stillAhead (plan))
        plan = {};

    // The stems can arrive late in the track, after the phrase the vocals
    // would have moved the mix to; the plan without them stands then.
    if (vocalAware && plan.smart)
        if (const auto withVocals = automix::planMix (*outgoing, *incoming, vocalsOn (onAirDeck), vocalsOn (1 - onAirDeck));
            stillAhead (withVocals))
            plan = withVocals;
}

void AutoMix::restoreVocals()
{
    if (vocalHandoffBar < 0)
        return;

    engine.getDeck (fadeOutgoingDeck).setStemGain (Stem::vocals, outgoingVocals);
    vocalHandoffBar = -1;
}

void AutoMix::restoreLowEq()
{
    if (! swappingLows)
        return;

    auto& mixer = engine.getMixer();
    mixer.setChannelEq (fadeOutgoingDeck, 0, outgoingLow);
    mixer.setChannelEq (fadeIncomingDeck, 0, incomingLow);
    swappingLows = false;
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

    restoreLowEq();
    restoreVocals();
    plan = {};
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
    restoreLowEq();
    restoreVocals();

    // Key lock goes back to how the user had it, except on a deck still
    // playing, where turning it off would be heard as the pitch jumping.
    for (int i = 0; i < 2; ++i)
    {
        if (keyLockBefore[(size_t) i] >= 0 && ! engine.getDeck (i).isPlaying())
            engine.getDeck (i).setKeyLock (keyLockBefore[(size_t) i] != 0);

        keyLockBefore[(size_t) i] = -1;
    }

    plan = {};
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
        requestStems (deckIndex);
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
        requestStems (deckIndex);
        planNextMix();
        changed();
    });
}

void AutoMix::beginFade (double nowSeconds, int outgoing, int incoming)
{
    // A skip mixes now, wherever the phrase is, so it gets the plain fade.
    const auto smartFade = plan.smart && ! skipRequested;

    // Started from the top of its intro, so its phrases begin with the
    // outgoing track's outro. syncDeck then moves it to the beat nearest
    // that point at the same phase as the leader, which keeps bar on bar.
    if (smartFade)
        engine.getDeck (incoming).seekToSeconds (plan.inSeconds);

    // Tempo and beat matched to what is playing, where both have a grid; a
    // track without one just fades in at its own speed. Key lock goes on
    // before it is heard, so the match does not drag it out of key.
    engine.syncDeck (incoming, outgoing, maxSyncStretch);

    if (auto& deck = engine.getDeck (incoming);
        keyLock && automix::needsKeyLock (deck.getTempoRatio()) && ! deck.isKeyLockEnabled())
    {
        if (keyLockBefore[(size_t) incoming] < 0)
            keyLockBefore[(size_t) incoming] = 0;

        deck.setKeyLock (true);
    }

    engine.getDeck (incoming).play();

    const auto& out = engine.getDeck (outgoing);
    const auto& in = engine.getDeck (incoming);
    auto wanted = fadeSeconds;

    if (const auto analysis = out.getAnalysis(); smartFade && analysis != nullptr)
        wanted = plan.fadeBeats * analysis->secondsPerBeat() / juce::jmax (0.05, out.getTempoRatio());

    fadeLength = automix::effectiveFadeSeconds (wanted, out.getLengthSeconds(), in.getLengthSeconds());
    fadeIncomingDeck = incoming;

    if (smartFade)
    {
        outgoingLow = engine.getMixer().getChannelEq (outgoing, 0);
        incomingLow = engine.getMixer().getChannelEq (incoming, 0);
        swappingLows = true;
    }

    // Two singers who could not be kept apart: the outgoing one stops by the
    // bar the incoming one starts, with the rest of that track still playing.
    if (smartFade && plan.vocalHandoffBar >= 0 && out.hasStems())
    {
        vocalHandoffBar = plan.vocalHandoffBar;
        fadeBars = plan.fadeBeats / (double) automix::beatsPerBar;
        outgoingVocals = out.getStemGain (Stem::vocals);
    }

    plan = {};
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
    restoreLowEq();
    restoreVocals();

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

    if (phase == Phase::playing && nextReady && onAirDeck >= 0
        && (plannedOutgoing.lock() != engine.getDeck (onAirDeck).getAnalysis()
            || plannedIncoming.lock() != engine.getDeck (1 - onAirDeck).getAnalysis()))
    {
        planNextMix();
        changed();
    }

    if (phase == Phase::fading)
    {
        const auto progress = fadeLength > 0.0 ? (nowSeconds - fadeStartSeconds) / fadeLength : 1.0;
        engine.getMixer().setCrossfaderPosition (automix::fadePosition (progress, fadeFrom, fadeTo));

        if (swappingLows)
        {
            engine.getMixer().setChannelEq (fadeOutgoingDeck, 0, automix::outgoingLowEq (progress, outgoingLow));
            engine.getMixer().setChannelEq (fadeIncomingDeck, 0, automix::incomingLowEq (progress, incomingLow));
        }

        if (vocalHandoffBar >= 0)
            engine.getDeck (fadeOutgoingDeck).setStemGain (
                Stem::vocals, automix::outgoingVocalGain (progress * fadeBars, vocalHandoffBar, outgoingVocals));

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

    // Smart: the fade starts at the planned phrase rather than a fade's
    // length before the end.
    const auto mixAt = plan.smart ? plan.outSeconds : onAir.getLengthSeconds();
    const auto lead = plan.smart ? 0.0 : fadeSeconds;

    automix::Situation situation;
    situation.remainingSeconds = (mixAt - onAir.getPositionSeconds()) / speed;
    situation.haveNext = getNextIndex() >= 0;
    situation.nextReady = nextReady;
    situation.nextLoading = nextLoading;
    situation.skipRequested = skipRequested;

    switch (automix::decide (situation, lead))
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
