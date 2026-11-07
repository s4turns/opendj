/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/Deck.h"

#include "analysis/TrackAnalyser.h"
#include "analysis/TrackDecoder.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>

namespace opendj
{

namespace
{
    // Four point Hermite interpolation. Random access at a fractional index is
    // what lets the same code path serve normal playback, tempo changes and, in
    // time, jog wheel scratching including reverse.
    inline float interpolate (const float* data, juce::int64 numSamples, double position) noexcept
    {
        const auto i1 = static_cast<juce::int64> (std::floor (position));
        const auto frac = static_cast<float> (position - static_cast<double> (i1));

        const auto at = [data, numSamples] (juce::int64 i) noexcept -> float
        {
            return (i < 0 || i >= numSamples) ? 0.0f : data[i];
        };

        const auto xm1 = at (i1 - 1);
        const auto x0  = at (i1);
        const auto x1  = at (i1 + 1);
        const auto x2  = at (i1 + 2);

        const auto c = (x1 - xm1) * 0.5f;
        const auto v = x0 - x1;
        const auto w = c + v;
        const auto a = w + v + (x2 - x0) * 0.5f;
        const auto bNeg = w + a;

        return (((a * frac) - bNeg) * frac + c) * frac + x0;
    }

    constexpr double gainRampSeconds = 0.005;

    // The level falls over the end of a brake or backspin. A rate close to zero
    // is far below anything a speaker can play, and would otherwise be heard
    // only as the thump of the signal stopping wherever it happened to be.
    constexpr double spinFadeSeconds = 0.1;

    constexpr double landingFadeSeconds = 0.005;

    /** One channel of the track at a fractional position: the recorded mix, or
        the stems weighted by their gains when the track has been separated.
        Summing the stems at unity gives the mix back, so the two paths agree;
        the mix is used when it can be, because it is a quarter of the work. */
    inline float readSample (const float* const* mix,
                             const SeparatedTrack* separation,
                             const float* stemGains,
                             int channel,
                             juce::int64 numSamples,
                             double position) noexcept
    {
        if (separation == nullptr)
            return interpolate (mix[channel], numSamples, position);

        float sum = 0.0f;

        for (int stem = 0; stem < numStems; ++stem)
        {
            // A silenced stem is the common case once a pad is pressed, and it
            // costs nothing to skip the interpolation for it.
            if (stemGains[stem] <= 0.0f)
                continue;

            sum += interpolate (separation->stems[(size_t) stem].getReadPointer (channel),
                                numSamples, position) * stemGains[stem];
        }

        return sum;
    }

    // Shorter than this and a loop is a tone rather than a rhythm, and a badly
    // placed loop-out could otherwise wrap the read head thousands of times in
    // one block.
    constexpr double minimumLoopSeconds = 0.02;

    // A record at 33 1/3 rpm takes 1.8 seconds to go round once.
    constexpr double vinylRevolutionSeconds = 60.0 / (100.0 / 3.0);

    // A nudge off the platter is measured as a fraction of a turn, not in ticks:
    // a wheel reporting absolute position counts in thousands per revolution and
    // one sending relative ticks in hundreds, and the same hand movement has to
    // mean the same thing on both. A fifth of a turn is about a five percent
    // bend, which is the range a DJ nudges by. It fades over about a quarter of
    // a second.
    constexpr double bendPerRevolution = 0.256;
    constexpr double bendDecayPerSecond = 0.002;

    // How long the head takes to close the distance to where the hand has put
    // the platter, in blocks. Messages from a wheel do not arrive evenly: during
    // the slow part of a scratch, which is exactly the turnaround, several
    // blocks pass with nothing and then two arrive together. Chasing the hand
    // over a couple of blocks bridges those gaps, where using only the ticks
    // that landed in this block makes the deck lurch and stall. The cost is a
    // lag of about two blocks, some ten milliseconds, which a hand cannot feel.
    constexpr double scratchChaseBlocks = 2.0;

    // Key lock. The R2 engine is the one that keeps up with a tempo fader in
    // real time at a cost of a few percent of a core per deck; R3
    // (OptionEngineFiner) sounds better on sustained material but wants several
    // times the CPU and adds latency. Swap the engine flag to try it.
    constexpr int stretchOptions = RubberBand::RubberBandStretcher::OptionProcessRealTime
                                 | RubberBand::RubberBandStretcher::OptionEngineFaster
                                 | RubberBand::RubberBandStretcher::OptionPitchHighConsistency;

    // How much audio the stretcher is handed at a time. Its maximum, so the
    // stretcher can size its own buffers once and never again.
    constexpr int stretchChunkSize = 1024;

    // Bounds on how many feed and retrieve rounds one block may take. The loop
    // always terminates on its own; this is the guarantee that it does so
    // within a bounded time even if the stretcher misbehaves.
    constexpr int stretchRoundsPerBlock = 64;
}

Deck::Deck (int deckIndex, juce::AudioFormatManager& formatManagerToUse)
    : index (deckIndex), formatManager (formatManagerToUse)
{
    for (auto& cue : hotCues)
        cue.store (-1.0, std::memory_order_relaxed);

    for (auto& gain : stemGains)
        gain.store (1.0f, std::memory_order_relaxed);
}

//==============================================================================
// Stems
//==============================================================================

void Deck::setSeparation (std::shared_ptr<const SeparatedTrack> separation, const juce::File& forFile)
{
    // A separation takes far longer than a load, so by the time one arrives the
    // deck may be playing something else entirely. Playing it then would be a
    // different record over this one.
    if (getLoadedFile() != forFile)
        return;

    if (separation != nullptr && ! separation->isWellFormed())
        return;

    auto* raw = separation.get();
    auto previous = std::atomic_exchange (&ownedSeparation, std::move (separation));

    activeSeparation.store (raw, std::memory_order_release);

    if (previous != nullptr)
        retiredSeparations.emplace_back (std::move (previous),
                                         blocksProcessed.load (std::memory_order_relaxed));
}

void Deck::setStemGain (Stem stem, float gain)
{
    const auto index = static_cast<size_t> (stem);

    if (index < numStems)
        stemGains[index].store (juce::jlimit (0.0f, 2.0f, gain), std::memory_order_relaxed);
}

float Deck::getStemGain (Stem stem) const
{
    const auto index = static_cast<size_t> (stem);
    return index < numStems ? stemGains[index].load (std::memory_order_relaxed) : 1.0f;
}

Deck::~Deck()
{
    activeTrack.store (nullptr, std::memory_order_release);
    activeSeparation.store (nullptr, std::memory_order_release);
    owned.reset();
    ownedSeparation.reset();
    retired.clear();
    retiredSeparations.clear();
}

//==============================================================================
// Loading
//==============================================================================

bool Deck::loadFile (const juce::File& file, const KnownTrack* known)
{
    juce::String reason;
    auto decoded = TrackDecoder::decode (formatManager, file, &reason);

    {
        std::lock_guard<std::mutex> lock (loadErrorMutex);
        lastLoadError = reason;
    }

    if (decoded == nullptr)
        return false;

    auto track = std::make_unique<Track>();
    track->file = file;
    track->sampleRate = decoded->sampleRate;
    track->audio = std::move (decoded->audio);
    track->title = known != nullptr && known->title.isNotEmpty()
        ? (known->artist.isNotEmpty() ? known->artist + " - " + known->title : known->title)
        : file.getFileNameWithoutExtension();

    // Waveform peaks and the beat grid are built here, on whichever thread is
    // doing the loading, so the track is fully described the moment it appears.
    // A track the library has seen before skips the tempo pass, which is the
    // slow part.
    auto analysis = known != nullptr && known->analysed
        ? TrackAnalyser::withKnownTempo (track->audio, track->sampleRate,
                                         known->bpm, known->firstBeatSeconds,
                                         known->tempoConfidence, {}, known->key)
        : TrackAnalyser::analyse (track->audio, track->sampleRate);

    // A first analysis detects a key whether or not the tags already have one.
    // The tags win, which is what the library shows too.
    if (known != nullptr && ! known->analysed && known->key.isValid() && analysis != nullptr)
    {
        auto withTaggedKey = std::make_shared<TrackAnalysis> (*analysis);
        withTaggedKey->key = known->key;
        analysis = std::move (withTaggedKey);
    }

    publish (std::move (track));
    loadedBpm.store (analysis != nullptr ? analysis->bpm : 0.0, std::memory_order_relaxed);
    loadedFirstBeatSeconds.store (analysis != nullptr ? analysis->firstBeatSeconds : 0.0, std::memory_order_relaxed);
    std::atomic_store (&analysisData, std::move (analysis));

    {
        std::lock_guard<std::mutex> lock (tapMutex);
        taps.reset();
    }

    // After publish(), which empties the pads for the new track.
    if (known != nullptr)
        for (int slot = 0; slot < numHotCues; ++slot)
            hotCues[(size_t) slot].store (known->hotCues[(size_t) slot], std::memory_order_relaxed);

    static std::atomic<juce::uint64> nextSerial { 0 };
    loadSerial.store (++nextSerial, std::memory_order_relaxed);
    return true;
}

void Deck::publish (std::unique_ptr<Track> newTrack)
{
    pause();

    const auto seconds = newTrack != nullptr
        ? static_cast<double> (newTrack->audio.getNumSamples()) / newTrack->sampleRate
        : 0.0;

    auto* raw = newTrack.get();
    auto previous = std::move (owned);
    owned = std::move (newTrack);

    for (auto& cue : hotCues)
        cue.store (-1.0, std::memory_order_relaxed);

    // The stems belonged to the track being replaced, so they go with it.
    activeSeparation.store (nullptr, std::memory_order_release);

    if (auto previousSeparation = std::atomic_exchange (&ownedSeparation, std::shared_ptr<const SeparatedTrack>());
        previousSeparation != nullptr)
        retiredSeparations.emplace_back (std::move (previousSeparation),
                                         blocksProcessed.load (std::memory_order_relaxed));

    // A loop belongs to the track it was made in. Carrying one across would
    // trap the new track between two positions that mean nothing in it.
    rollActive.store (false, std::memory_order_relaxed);
    slipPositionSeconds.store (-1.0, std::memory_order_relaxed);
    reverseEnabled.store (false, std::memory_order_relaxed);
    censorHeld.store (false, std::memory_order_relaxed);
    spinRequest.store (spinNone, std::memory_order_relaxed);
    loopEnabled.store (false, std::memory_order_relaxed);
    loopStartSeconds.store (-1.0, std::memory_order_relaxed);
    loopEndSeconds.store (-1.0, std::memory_order_relaxed);
    loopBeats.store (0.0, std::memory_order_relaxed);

    // A new track plays at its own speed. The tempo belonged to the last one:
    // a deck matched to another and then loaded with something else kept the
    // stretch, and the new track came up fast or slow until somebody noticed.
    tempoRatio.store (1.0, std::memory_order_relaxed);
    keyShift.store (0, std::memory_order_relaxed);

    lengthSeconds.store (seconds, std::memory_order_relaxed);
    cuePointSeconds.store (0.0, std::memory_order_relaxed);
    positionSeconds.store (0.0, std::memory_order_relaxed);
    pendingSeekSeconds.store (0.0, std::memory_order_relaxed);

    activeTrack.store (raw, std::memory_order_release);

    if (previous != nullptr)
    {
        previous->retiredAtBlock = blocksProcessed.load (std::memory_order_relaxed);
        retired.push_back (std::move (previous));
    }
}

void Deck::unload()
{
    publish (nullptr);
    std::atomic_store (&analysisData, std::shared_ptr<const TrackAnalysis>());
    loadSerial.store (0, std::memory_order_relaxed);
}

juce::String Deck::getLastLoadError() const
{
    std::lock_guard<std::mutex> lock (loadErrorMutex);
    return lastLoadError;
}

std::shared_ptr<const TrackAnalysis> Deck::getAnalysis() const
{
    return std::atomic_load (&analysisData);
}

std::shared_ptr<const SeparatedTrack> Deck::getSeparation() const
{
    return std::atomic_load (&ownedSeparation);
}

void Deck::cleanUp()
{
    const auto now = blocksProcessed.load (std::memory_order_relaxed);

    const auto separationSafeToFree = [now] (const auto& entry)
    {
        return now - entry.second >= 2;
    };

    retiredSeparations.erase (std::remove_if (retiredSeparations.begin(), retiredSeparations.end(),
                                              separationSafeToFree),
                              retiredSeparations.end());

    // Two blocks is enough: the audio thread reloads the pointer at the top of
    // every processBlock() call, so it cannot still hold a stale one by then.
    const auto safeToFree = [now] (const std::unique_ptr<Track>& t)
    {
        return now - t->retiredAtBlock >= 2;
    };

    retired.erase (std::remove_if (retired.begin(), retired.end(), safeToFree), retired.end());
}

juce::File Deck::getLoadedFile() const
{
    auto* track = activeTrack.load (std::memory_order_acquire);
    return track != nullptr ? track->file : juce::File();
}

juce::String Deck::getTrackTitle() const
{
    auto* track = activeTrack.load (std::memory_order_acquire);
    return track != nullptr ? track->title : juce::String();
}

//==============================================================================
// Transport
//==============================================================================

void Deck::play()
{
    if (! isLoaded())
        return;

    // Play during a brake catches the record again.
    if (isSpinningDown())
        spinRequest.store (spinCancel, std::memory_order_relaxed);

    playing.store (true, std::memory_order_relaxed);
}

void Deck::setKeyShift (int semitones)
{
    keyShift.store (juce::jlimit (-maximumKeyShift, maximumKeyShift, semitones), std::memory_order_relaxed);
}

MusicalKey Deck::getEffectiveKey() const
{
    const auto analysis = getAnalysis();

    if (analysis == nullptr || ! analysis->hasKey())
        return {};

    auto semitones = getKeyShift();

    if (! isKeyLockEnabled())
        semitones += juce::roundToInt (12.0 * std::log2 (juce::jmax (0.01, getTempoRatio())));

    return analysis->key.transposed (semitones);
}

void Deck::brake()
{
    if (isPlaying() && ! isSpinningDown())
        spinRequest.store (spinBrake, std::memory_order_relaxed);
}

void Deck::backspin()
{
    if (isPlaying() && ! isSpinningDown())
        spinRequest.store (spinBackspin, std::memory_order_relaxed);
}

void Deck::setReverse (bool shouldReverse)
{
    reverseEnabled.store (shouldReverse && isLoaded(), std::memory_order_relaxed);
}

void Deck::toggleReverse()
{
    setReverse (! isReverseEnabled());
}

void Deck::setCensor (bool shouldCensor)
{
    censorHeld.store (shouldCensor && isLoaded(), std::memory_order_relaxed);
}

void Deck::pause()
{
    playing.store (false, std::memory_order_relaxed);
    previewingFromCue.store (false, std::memory_order_relaxed);
}

void Deck::togglePlay()
{
    if (isPlaying())
        pause();
    else
        play();
}

void Deck::cuePressed()
{
    if (! isLoaded())
        return;

    if (isPlaying())
    {
        // Playing: drop back to the cue point and stop there.
        pause();
        seekToSeconds (getCueSeconds());
        return;
    }

    // Stopped away from the cue point: this is where the cue point now goes.
    const auto position = getPositionSeconds();

    if (std::abs (position - getCueSeconds()) > 0.001)
    {
        const auto cue = quantized (position);
        cuePointSeconds.store (cue, std::memory_order_relaxed);

        if (std::abs (cue - position) > 0.0005)
            seekToSeconds (cue);
    }
    else
        seekToSeconds (getCueSeconds());

    previewingFromCue.store (true, std::memory_order_relaxed);
    playing.store (true, std::memory_order_relaxed);
}

void Deck::cueReleased()
{
    if (! previewingFromCue.exchange (false, std::memory_order_relaxed))
        return;

    playing.store (false, std::memory_order_relaxed);
    seekToSeconds (getCueSeconds());
}

void Deck::seekToSeconds (double seconds)
{
    const auto length = getLengthSeconds();
    pendingSeekSeconds.store (juce::jlimit (0.0, juce::jmax (0.0, length), seconds),
                              std::memory_order_relaxed);
}

void Deck::seekToFraction (double proportion)
{
    seekToSeconds (juce::jlimit (0.0, 1.0, proportion) * getLengthSeconds());
}

bool Deck::beatJump (double beats)
{
    if (! isLoaded() || beats == 0.0)
        return false;

    const auto analysis = getAnalysis();

    if (analysis == nullptr || ! analysis->hasTempo())
        return false;

    const auto offset = beats * analysis->secondsPerBeat();
    const auto length = getLengthSeconds();

    // A seek that has not reached the audio thread yet is where the playhead
    // is about to be, so two quick presses add up rather than the second one
    // being measured from where the first started.
    const auto pending = pendingSeekSeconds.load (std::memory_order_relaxed);
    const auto from = pending >= 0.0 ? pending : getPositionSeconds();
    const auto target = from + offset;

    if (target < 0.0 || target >= length)
        return false;

    if (isLoopEnabled() && hasLoop())
    {
        const auto start = loopStartSeconds.load (std::memory_order_relaxed) + offset;
        const auto end = loopEndSeconds.load (std::memory_order_relaxed) + offset;

        if (start >= 0.0 && end <= length)
        {
            loopStartSeconds.store (start, std::memory_order_relaxed);
            loopEndSeconds.store (end, std::memory_order_relaxed);
        }
        else
        {
            loopEnabled.store (false, std::memory_order_relaxed);
        }
    }

    seekToSeconds (target);
    return true;
}

//==============================================================================
// Beat grid editing
//==============================================================================

bool Deck::setBeatGrid (double bpm, double firstBeatSeconds)
{
    if (! isLoaded() || ! (bpm >= minimumGridBpm && bpm <= maximumGridBpm))
        return false;

    auto current = getAnalysis();

    if (current == nullptr)
        return false;

    // Within the first bar, not the first beat: wrapping by a beat would lose
    // which beat of the bar is the downbeat.
    const auto bar = 4.0 * 60.0 / bpm;
    auto first = std::fmod (firstBeatSeconds, bar);

    if (first < 0.0)
        first += bar;

    auto edited = std::make_shared<TrackAnalysis> (*current);
    edited->bpm = bpm;
    edited->firstBeatSeconds = first;
    edited->confidence = 1.0f;   // a person said so

    // Only over the analysis the edit was made from: if another track loaded
    // in between, this edit belongs to the one that left and is dropped.
    std::shared_ptr<const TrackAnalysis> replacement = std::move (edited);

    if (! std::atomic_compare_exchange_strong (&analysisData, &current, replacement))
        return false;

    if (onBeatGridChanged != nullptr)
        onBeatGridChanged (getLoadedFile(), bpm, first);

    return true;
}

bool Deck::retempoAroundPlayhead (double bpm)
{
    const auto analysis = getAnalysis();

    if (analysis == nullptr)
        return false;

    const auto position = getPositionSeconds();

    // A track with no grid yet starts one at the playhead.
    if (! analysis->hasTempo())
        return setBeatGrid (bpm, position);

    // Count the beats from the anchor to the one nearest the playhead, and lay
    // the same count back from it at the new tempo, so that beat stays put and
    // so does its place in the bar.
    const auto period = analysis->secondsPerBeat();
    const auto beats = std::round ((position - analysis->firstBeatSeconds) / period);
    const auto pivot = analysis->firstBeatSeconds + beats * period;

    return setBeatGrid (bpm, pivot - beats * 60.0 / bpm);
}

bool Deck::adjustGridBpm (double deltaBpm)
{
    const auto analysis = getAnalysis();
    return analysis != nullptr && analysis->hasTempo() && retempoAroundPlayhead (analysis->bpm + deltaBpm);
}

bool Deck::scaleGridBpm (double factor)
{
    const auto analysis = getAnalysis();
    return analysis != nullptr && analysis->hasTempo() && factor > 0.0
        && retempoAroundPlayhead (analysis->bpm * factor);
}

bool Deck::shiftGrid (double seconds)
{
    const auto analysis = getAnalysis();
    return analysis != nullptr && analysis->hasTempo()
        && setBeatGrid (analysis->bpm, analysis->firstBeatSeconds + seconds);
}

bool Deck::setDownbeatHere()
{
    const auto analysis = getAnalysis();
    return analysis != nullptr && analysis->hasTempo()
        && setBeatGrid (analysis->bpm, getPositionSeconds());
}

double Deck::tapTempo (double nowSeconds)
{
    if (! isLoaded())
        return 0.0;

    double bpm;

    {
        std::lock_guard<std::mutex> lock (tapMutex);
        bpm = taps.tap (nowSeconds);
    }

    if (bpm <= 0.0)
        return 0.0;

    // Two decimals is as fine as anyone taps, and it keeps the readout calm.
    bpm = std::round (juce::jlimit (minimumGridBpm, maximumGridBpm, bpm) * 100.0) / 100.0;
    retempoAroundPlayhead (bpm);
    return bpm;
}

bool Deck::revertBeatGrid()
{
    const auto bpm = loadedBpm.load (std::memory_order_relaxed);

    if (bpm <= 0.0)
        return false;

    return setBeatGrid (bpm, loadedFirstBeatSeconds.load (std::memory_order_relaxed));
}

bool Deck::isBeatGridEdited() const
{
    const auto analysis = getAnalysis();

    if (analysis == nullptr)
        return false;

    return std::abs (analysis->bpm - loadedBpm.load (std::memory_order_relaxed)) > 1.0e-6
        || std::abs (analysis->firstBeatSeconds - loadedFirstBeatSeconds.load (std::memory_order_relaxed)) > 1.0e-6;
}

double Deck::quantized (double seconds) const
{
    if (! isQuantizeEnabled())
        return seconds;

    const auto analysis = getAnalysis();

    if (analysis == nullptr || ! analysis->hasTempo())
        return seconds;

    return juce::jlimit (0.0, juce::jmax (0.0, getLengthSeconds()),
                         analysis->nearestBeatSeconds (seconds));
}

void Deck::setTempoRatio (double ratio)
{
    tempoRatio.store (juce::jlimit (0.05, 4.0, ratio), std::memory_order_relaxed);
}

void Deck::setTrim (float linearGain)
{
    trimGain.store (juce::jlimit (0.0f, 4.0f, linearGain), std::memory_order_relaxed);
}

void Deck::setKeyLock (bool shouldLock)
{
    keyLock.store (shouldLock, std::memory_order_relaxed);
}

void Deck::toggleKeyLock()
{
    keyLock.store (! keyLock.load (std::memory_order_relaxed), std::memory_order_relaxed);
}

//==============================================================================
// Hot cues
//==============================================================================

void Deck::hotCuePressed (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numHotCues) || ! isLoaded())
        return;

    auto& cue = hotCues[(size_t) slot];

    if (cue.load (std::memory_order_relaxed) < 0.0)
    {
        cue.store (quantized (getPositionSeconds()), std::memory_order_relaxed);

        if (onHotCuesChanged != nullptr)
            onHotCuesChanged (getLoadedFile(), getHotCues());
    }
    else
    {
        seekToSeconds (cue.load (std::memory_order_relaxed));
    }
}

void Deck::clearHotCue (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numHotCues))
        return;

    if (hotCues[(size_t) slot].exchange (-1.0, std::memory_order_relaxed) >= 0.0
        && onHotCuesChanged != nullptr && isLoaded())
        onHotCuesChanged (getLoadedFile(), getHotCues());
}

double Deck::getHotCueSeconds (int slot) const
{
    return juce::isPositiveAndBelow (slot, numHotCues)
        ? hotCues[(size_t) slot].load (std::memory_order_relaxed)
        : -1.0;
}

bool Deck::hasHotCue (int slot) const
{
    return getHotCueSeconds (slot) >= 0.0;
}

HotCues Deck::getHotCues() const
{
    HotCues cues;

    for (int slot = 0; slot < numHotCues; ++slot)
        cues[(size_t) slot] = hotCues[(size_t) slot].load (std::memory_order_relaxed);

    return cues;
}

//==============================================================================
// Loops
//==============================================================================

bool Deck::hasLoop() const noexcept
{
    const auto start = loopStartSeconds.load (std::memory_order_relaxed);
    const auto end = loopEndSeconds.load (std::memory_order_relaxed);

    return start >= 0.0 && end > start;
}

void Deck::setLoopIn()
{
    if (! isLoaded())
        return;

    loopStartSeconds.store (quantized (getPositionSeconds()), std::memory_order_relaxed);
    loopBeats.store (0.0, std::memory_order_relaxed);
}

void Deck::setLoopOut()
{
    if (! isLoaded())
        return;

    const auto start = loopStartSeconds.load (std::memory_order_relaxed);
    const auto end = quantized (getPositionSeconds());

    if (start < 0.0 || end - start < minimumLoopSeconds)
        return;

    loopEndSeconds.store (end, std::memory_order_relaxed);
    loopBeats.store (0.0, std::memory_order_relaxed);
    loopEnabled.store (true, std::memory_order_relaxed);
}

bool Deck::setLoopBeats (double beats)
{
    if (! isLoaded() || beats <= 0.0)
        return false;

    const auto analysis = getAnalysis();

    if (analysis == nullptr || ! analysis->hasTempo())
        return false;

    const auto length = beats * analysis->secondsPerBeat();

    if (length < minimumLoopSeconds)
        return false;

    // Snap to the beat at or behind the playhead. Snapping to the nearest beat
    // instead would let a loop start a fraction of a beat late, which is exactly
    // the mistake the button exists to prevent.
    const auto start = analysis->beatAtOrBehindSeconds (getPositionSeconds());

    loopStartSeconds.store (start, std::memory_order_relaxed);
    loopEndSeconds.store (start + length, std::memory_order_relaxed);
    loopBeats.store (beats, std::memory_order_relaxed);
    loopEnabled.store (true, std::memory_order_relaxed);
    return true;
}

void Deck::halveLoop()
{
    if (! hasLoop())
        return;

    const auto start = loopStartSeconds.load (std::memory_order_relaxed);
    const auto length = loopEndSeconds.load (std::memory_order_relaxed) - start;

    if (length * 0.5 < minimumLoopSeconds)
        return;

    loopEndSeconds.store (start + length * 0.5, std::memory_order_relaxed);

    if (const auto beats = loopBeats.load (std::memory_order_relaxed); beats > 0.0)
        loopBeats.store (beats * 0.5, std::memory_order_relaxed);
}

void Deck::doubleLoop()
{
    if (! hasLoop())
        return;

    const auto start = loopStartSeconds.load (std::memory_order_relaxed);
    const auto length = loopEndSeconds.load (std::memory_order_relaxed) - start;
    const auto trackLength = getLengthSeconds();

    if (trackLength > 0.0 && start + length * 2.0 > trackLength)
        return;

    loopEndSeconds.store (start + length * 2.0, std::memory_order_relaxed);

    if (const auto beats = loopBeats.load (std::memory_order_relaxed); beats > 0.0)
        loopBeats.store (beats * 2.0, std::memory_order_relaxed);
}

bool Deck::toggleLoopBeats (double beats)
{
    if (isLoopEnabled() && std::abs (getLoopBeats() - beats) < 0.001)
    {
        setLoopEnabled (false);
        return false;
    }

    return setLoopBeats (beats);
}

void Deck::setLoopEnabled (bool shouldLoop)
{
    loopEnabled.store (shouldLoop && hasLoop(), std::memory_order_relaxed);
}

void Deck::toggleLoop()
{
    setLoopEnabled (! isLoopEnabled());
}

void Deck::clearLoop()
{
    loopEnabled.store (false, std::memory_order_relaxed);
    loopStartSeconds.store (-1.0, std::memory_order_relaxed);
    loopEndSeconds.store (-1.0, std::memory_order_relaxed);
    loopBeats.store (0.0, std::memory_order_relaxed);
}

void Deck::reloop()
{
    if (! hasLoop())
        return;

    seekToSeconds (loopStartSeconds.load (std::memory_order_relaxed));
    loopEnabled.store (true, std::memory_order_relaxed);
}

bool Deck::beginLoopRoll (double beats)
{
    if (isLoopRolling() || ! setLoopBeats (beats))
        return false;

    rollActive.store (true, std::memory_order_relaxed);
    return true;
}

void Deck::endLoopRoll()
{
    if (! rollActive.exchange (false, std::memory_order_relaxed))
        return;

    // The loop was the roll's doing, so it goes when the roll goes. Leaving it
    // behind enabled would silently trap the deck.
    clearLoop();
}

void Deck::setSlipEnabled (bool shouldSlip)
{
    slipEnabled.store (shouldSlip, std::memory_order_relaxed);
}

void Deck::toggleSlip()
{
    setSlipEnabled (! isSlipEnabled());
}

bool Deck::wrapIntoLoop (double& position, double rate, const Track& track) const
{
    if (! loopEnabled.load (std::memory_order_relaxed))
        return false;

    const auto start = loopStartSeconds.load (std::memory_order_relaxed);
    const auto end = loopEndSeconds.load (std::memory_order_relaxed);

    if (start < 0.0 || end - start < minimumLoopSeconds)
        return false;

    const auto startSample = start * track.sampleRate;
    const auto endSample = end * track.sampleRate;
    const auto length = endSample - startSample;

    // Modulo rather than a loop that subtracts: a very short loop at a high
    // tempo could otherwise want hundreds of iterations, and the audio thread
    // should not be doing an unbounded amount of anything.
    if (rate >= 0.0 && position >= endSample)
    {
        position = startSample + std::fmod (position - startSample, length);
        return true;
    }

    // Running backwards out of the front of a loop wraps to its end, so a
    // reversed deck inside a loop stays inside it.
    if (rate < 0.0 && position < startSample)
    {
        position = endSample - std::fmod (endSample - position, length);
        return true;
    }

    return false;
}

//==============================================================================
// Jog wheel
//==============================================================================

void Deck::setJogTouched (bool touched)
{
    if (jogTouched.exchange (touched, std::memory_order_relaxed) != touched)
        jogTicks.store (0.0, std::memory_order_relaxed);   // start from a clean slate, either way
}

void Deck::addJogTicks (double ticks)
{
    // Accumulate rather than overwrite: several MIDI messages can arrive
    // between two audio blocks, and dropping any of them would make the platter
    // feel like it was slipping.
    auto current = jogTicks.load (std::memory_order_relaxed);

    while (! jogTicks.compare_exchange_weak (current, current + ticks, std::memory_order_relaxed))
        ;
}

void Deck::setJogTicksPerRevolution (int ticks)
{
    jogTicksPerRevolution.store (juce::jmax (1, ticks), std::memory_order_relaxed);
}

float Deck::readAndResetPeak() noexcept
{
    return peakLevel.exchange (0.0f, std::memory_order_relaxed);
}

//==============================================================================
// Audio thread
//==============================================================================

void Deck::prepare (double sampleRate, int)
{
    deviceSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    transportGain.reset (deviceSampleRate, gainRampSeconds);
    transportGain.setCurrentAndTargetValue (0.0f);

    for (size_t i = 0; i < numStems; ++i)
    {
        stemGainRamps[i].reset (deviceSampleRate, gainRampSeconds);
        stemGainRamps[i].setCurrentAndTargetValue (stemGains[i].load (std::memory_order_relaxed));
    }

    // Built here, before the device starts, so that processBlock() never has to.
    stretcher = std::make_unique<RubberBand::RubberBandStretcher> (
        static_cast<size_t> (deviceSampleRate), 2, stretchOptions, 1.0, 1.0);
    stretcher->setMaxProcessSize (static_cast<size_t> (stretchChunkSize));

    stretchInput.setSize (2, stretchChunkSize);
    stretchDiscard.setSize (2, stretchChunkSize);
    stretchPrimed = false;
}

void Deck::releaseResources()
{
    transportGain.setCurrentAndTargetValue (0.0f);
    stretchPrimed = false;
}

void Deck::processBlock (juce::AudioBuffer<float>& destination)
{
    destination.clear();
    blocksProcessed.fetch_add (1, std::memory_order_relaxed);

    auto* track = activeTrack.load (std::memory_order_acquire);
    const auto spinAsked = spinRequest.exchange (spinNone, std::memory_order_relaxed);

    if (track == nullptr)
    {
        spinActive.store (false, std::memory_order_relaxed);
        transportGain.setCurrentAndTargetValue (0.0f);
        peakLevel.store (0.0f, std::memory_order_relaxed);
        stretchPrimed = false;
        scratchTargetValid = false;
        return;
    }

    const auto numSamples = static_cast<juce::int64> (track->audio.getNumSamples());

    const auto scratching = jogTouched.load (std::memory_order_relaxed);
    const auto ticks = jogTicks.exchange (0.0, std::memory_order_relaxed);
    const auto blockSize = destination.getNumSamples();

    // A hand on the platter is as good as pressing play: you can scratch a
    // stopped deck, and you expect to hear it.
    const auto wantPlay = playing.load (std::memory_order_relaxed);
    const auto audible = wantPlay || scratching;
    transportGain.setTargetValue (audible ? 1.0f : 0.0f);

    const auto spinFadeSamples = spinFadeSeconds * deviceSampleRate;

    // Anything that takes the deck back ends a brake or backspin. The level
    // carries on from wherever the fade had reached, so it cannot click.
    if (spinActive.load (std::memory_order_relaxed)
        && (spinAsked == spinCancel || ! wantPlay || scratching))
    {
        const auto fade = static_cast<float> (juce::jmin (1.0, static_cast<double> (spinSamplesLeft) / spinFadeSamples));
        transportGain.setCurrentAndTargetValue (juce::jmin (transportGain.getCurrentValue(), fade));
        transportGain.setTargetValue (audible ? 1.0f : 0.0f);
        spinActive.store (false, std::memory_order_relaxed);
    }

    // A seek that arrives mid fade-out waits for the fade to finish. Pressing
    // cue while playing should cut the audio where it is and only then send the
    // head back, rather than stamping a fragment of the cue point over the tail.
    const auto fadingOut = ! audible && transportGain.getCurrentValue() > 0.0f;

    if (! fadingOut)
    {
        if (const auto seek = pendingSeekSeconds.exchange (-1.0, std::memory_order_relaxed); seek >= 0.0)
        {
            readPosition = seek * track->sampleRate;
            stretchPrimed = false;   // whatever the stretcher holds is from the old place
            scratchTargetValid = false;
            slipValid = false;       // the head was moved by hand; the shadow is stale
        }
    }

    // Once stopped and fully faded, hold position and cost nothing.
    if (! audible && transportGain.getCurrentValue() <= 0.0f)
    {
        pitchBend = 0.0;
        stretchPrimed = false;
        scratchTargetValid = false;

        // A stopped deck has no music running under it. A shadow head left over
        // from a censor would otherwise pull the next press of play back to
        // wherever the track had reached before it stopped.
        if (motionSlip)
        {
            motionSlip = false;
            slipValid = false;
            slipPositionSeconds.store (-1.0, std::memory_order_relaxed);
        }

        positionSeconds.store (readPosition / track->sampleRate, std::memory_order_relaxed);
        peakLevel.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const auto fileToDevice = track->sampleRate / deviceSampleRate;
    const auto tempoRate = fileToDevice * tempoRatio.load (std::memory_order_relaxed);
    double rate = tempoRate;

    if (scratching)
    {
        // One turn of the platter moves one turn of a record at 33 1/3 rpm, so
        // the wheel feels like vinyl rather than like a scrub bar. A still hand
        // is silence and a backwards hand plays backwards.
        const auto secondsPerTick = vinylRevolutionSeconds
                                  / jogTicksPerRevolution.load (std::memory_order_relaxed);

        // The hand's own position, which the head then chases. Keeping it apart
        // from the head is what lets an uneven stream of messages come out as
        // even movement.
        if (! scratchTargetValid)
        {
            scratchTarget = readPosition;
            scratchTargetValid = true;
        }

        scratchTarget += ticks * secondsPerTick * track->sampleRate;

        rate = (scratchTarget - readPosition) / (juce::jmax (1, blockSize) * scratchChaseBlocks);
        pitchBend = 0.0;
    }
    else
    {
        // Off the platter the hand's position means nothing, so forget it: the
        // next touch starts again from wherever the head has reached.
        scratchTargetValid = false;

        // A nudge bends the pitch briefly and then decays back to the tempo
        // fader, the way pushing the side of a record does.
        pitchBend += (ticks / juce::jmax (1, jogTicksPerRevolution.load (std::memory_order_relaxed)))
                   * bendPerRevolution;
        pitchBend *= std::pow (bendDecayPerSecond, blockSize / deviceSampleRate);

        if (std::abs (pitchBend) < 1.0e-5)
            pitchBend = 0.0;

        rate = tempoRate * (1.0 + pitchBend);
    }

    const auto reverseOn = reverseEnabled.load (std::memory_order_relaxed);
    const auto censoring = censorHeld.load (std::memory_order_relaxed) && wantPlay && ! scratching;
    const auto reversed = ! scratching && (reverseOn || censoring);

    if (reversed)
        rate = -rate;

    // A brake starts from the speed the deck is running at, in whichever
    // direction, and a backspin starts faster and the other way. Either walks
    // the rate to zero a sample at a time, because a step a block is heard as
    // the pitch moving in stairs.
    if ((spinAsked == spinBrake || spinAsked == spinBackspin)
        && wantPlay && ! scratching && ! spinActive.load (std::memory_order_relaxed))
    {
        const auto isBrake = spinAsked == spinBrake;
        const auto seconds = isBrake ? brakeSeconds : backspinSeconds;

        spinRate = isBrake ? rate : -rate * backspinSpeed;
        spinSamplesLeft = juce::jmax<juce::int64> (1, static_cast<juce::int64> (seconds * deviceSampleRate));
        spinRateStep = -spinRate / static_cast<double> (spinSamplesLeft);
        spinActive.store (true, std::memory_order_relaxed);
    }

    const auto spinning = spinActive.load (std::memory_order_relaxed);

    if (spinning)
        rate = spinRate;

    const auto trim = trimGain.load (std::memory_order_relaxed);
    const auto outChannels = juce::jmin (2, destination.getNumChannels());

    const float* source[2] = { track->audio.getReadPointer (0), track->audio.getReadPointer (1) };

    // Stems are only worth reading when one of them is turned down: at unity
    // they sum back to the mix that is already there, for four times the work.
    const auto* separation = activeSeparation.load (std::memory_order_acquire);
    float stemGainNow[numStems];
    auto anyStemChanged = false;

    for (size_t stem = 0; stem < numStems; ++stem)
    {
        stemGainRamps[stem].setTargetValue (stemGains[stem].load (std::memory_order_relaxed));
        anyStemChanged = anyStemChanged || stemGainRamps[stem].getTargetValue() != 1.0f
                                        || stemGainRamps[stem].getCurrentValue() != 1.0f;
        stemGainNow[stem] = stemGainRamps[stem].getCurrentValue();
    }

    if (! anyStemChanged)
        separation = nullptr;

    // Key lock is bypassed on the platter, and if it would have nothing to do:
    // at exactly the recorded speed the stretcher is a delay line, and a delay
    // that comes and goes with the fader is worse than a pitch that does. A
    // stretcher cannot run backwards or follow a brake either, and the falling
    // pitch is what a brake is.
    //
    // A key shift needs it whatever the speed, since that is the only thing
    // here that can move a pitch by itself. Without key lock, the tempo moves
    // the pitch on top of the shift, just as the fader always does.
    const auto keyLocked = keyLock.load (std::memory_order_relaxed);
    const auto shift = keyShift.load (std::memory_order_relaxed);
    const auto stretching = ! scratching
                         && ! reversed
                         && ! spinning
                         && stretcher != nullptr
                         && outChannels == 2
                         && (shift != 0 || (keyLocked && std::abs (rate - fileToDevice) > 1.0e-6));

    if (stretching)
    {
        const auto pitchScale = std::pow (2.0, shift / 12.0) * (keyLocked ? 1.0 : rate / fileToDevice);
        renderStretched (*track, destination, rate, fileToDevice, pitchScale, separation, stemGainNow);
    }
    else
        stretchPrimed = false;

    auto position = readPosition;
    auto peak = 0.0f;

    // Whether this is the last block before a censor, a reverse or a backspin
    // lands back on the timeline. Landing is a jump to another part of the track,
    // so the level comes down over the end of this block and back up after it,
    // rather than leaving a step in the waveform to be heard as a tick.
    const auto slipOn = slipEnabled.load (std::memory_order_relaxed);
    const auto landingNext = motionSlip && slipValid && ! censoring
                          && ! (slipOn && (reverseOn || spinning))
                          && ! rollActive.load (std::memory_order_relaxed)
                          && ! (slipOn && (scratching || loopEnabled.load (std::memory_order_relaxed)));
    const auto landingFadeSamples = juce::jmax (1.0, juce::jmin (static_cast<double> (blockSize),
                                                                 landingFadeSeconds * deviceSampleRate));

    for (int i = 0; i < blockSize; ++i)
    {
        const auto spinFade = spinning
            ? static_cast<float> (juce::jmin (1.0, static_cast<double> (spinSamplesLeft) / spinFadeSamples))
            : 1.0f;
        const auto landingFade = landingNext
            ? static_cast<float> (juce::jmin (1.0, static_cast<double> (blockSize - 1 - i) / landingFadeSamples))
            : 1.0f;
        const auto gain = transportGain.getNextValue() * trim * spinFade * landingFade;
        const auto inRange = position >= 0.0 && position < static_cast<double> (numSamples);

        for (size_t stem = 0; stem < numStems; ++stem)
            stemGainNow[stem] = stemGainRamps[stem].getNextValue();

        for (int ch = 0; ch < outChannels; ++ch)
        {
            const auto sample = stretching
                ? destination.getSample (ch, i) * gain
                : (inRange ? readSample (source, separation, stemGainNow, ch, numSamples, position) * gain
                           : 0.0f);

            destination.setSample (ch, i, sample);
            peak = juce::jmax (peak, std::abs (sample));
        }

        position += rate;

        // A hand on the platter beats a loop: direct manipulation should never
        // be fenced in by something set earlier.
        if (! scratching)
            wrapIntoLoop (position, rate, *track);

        if (spinning && spinSamplesLeft > 0)
        {
            rate = --spinSamplesLeft > 0 ? rate + spinRateStep : 0.0;
        }
    }

    if (spinning)
    {
        spinRate = rate;

        // Finished, and already silent from the fade. The ordinary fade-out
        // would run at full speed, which is a chirp at the end of a brake, so
        // the level is simply left at zero: a stopped deck stays quiet, and one
        // dropping back in with slip fades up from nothing.
        if (spinSamplesLeft <= 0)
        {
            spinActive.store (false, std::memory_order_relaxed);
            transportGain.setCurrentAndTargetValue (0.0f);

            if (! slipEnabled.load (std::memory_order_relaxed))
                playing.store (false, std::memory_order_relaxed);
        }
    }

    // While a roll is held, the track carries on underneath it. This is the
    // shadow head that says where it would have been, and it is what letting go
    // jumps to.
    // Anything that takes the deck off the timeline runs a shadow playhead
    // underneath at the tempo fader's rate, which is where the music would have
    // been. A roll always does this, because that is what a roll is; with slip
    // mode on, a hand on the platter and a running loop do it too.
    //
    // A censor always does too, since landing back on time is what it is for.
    // With slip on, a brake, a backspin and reverse join them.
    const auto motionOffTimeline = censoring
                                || (slipOn && ! scratching
                                    && (reverseOn || spinActive.load (std::memory_order_relaxed)));
    const auto offTimeline = rollActive.load (std::memory_order_relaxed)
                          || motionOffTimeline
                          || (slipOn && (scratching || loopEnabled.load (std::memory_order_relaxed)));

    if (motionOffTimeline)
        motionSlip = true;

    if (offTimeline)
    {
        if (! slipValid)
        {
            slipPosition = readPosition;
            slipValid = true;
        }

        // The tempo rate, deliberately, not the rate actually used: during a
        // scratch that is the hand's rate, and the whole point of the shadow is
        // that the hand did not move it.
        slipPosition += tempoRate * blockSize;
        slipPositionSeconds.store (slipPosition / track->sampleRate, std::memory_order_relaxed);
    }
    else if (slipValid)
    {
        // Just came back onto the timeline. Land where the music got to, not
        // where the hand or the loop left off, so a scratch or a roll can be
        // dropped in mid-phrase without losing the mix. Off the end of the
        // track it simply stops, as playing off the end always does.
        //
        // The shadow has not been moved for this block, and the music has.
        position = juce::jlimit (0.0, static_cast<double> (numSamples), slipPosition + tempoRate * blockSize);
        slipValid = false;

        if (landingNext)
            transportGain.setCurrentAndTargetValue (0.0f);   // and up again from here

        motionSlip = false;
        stretchPrimed = false;
        slipPositionSeconds.store (-1.0, std::memory_order_relaxed);
    }

    // Running off either end stops playback rather than looping. Scratching
    // just stops at the edge, since the hand is still on the platter, and so
    // does a censor, which comes back to the music when it is let go.
    if (position >= static_cast<double> (numSamples))
    {
        position = static_cast<double> (numSamples);

        if (! scratching)
            playing.store (false, std::memory_order_relaxed);
    }
    else if (position < 0.0)
    {
        position = 0.0;

        if (! scratching && ! censoring)
        {
            playing.store (false, std::memory_order_relaxed);

            // Left on, the next press of play would stop straight away.
            reverseEnabled.store (false, std::memory_order_relaxed);

            // Off the front is already silence, and the ordinary fade-out would
            // play a block of the opening forwards.
            transportGain.setCurrentAndTargetValue (0.0f);
        }
    }

    readPosition = position;
    positionSeconds.store (position / track->sampleRate, std::memory_order_relaxed);
    peakLevel.store (juce::jmax (peak, peakLevel.load (std::memory_order_relaxed)),
                     std::memory_order_relaxed);
}

//==============================================================================
// Key lock
//==============================================================================

void Deck::feedStretcher (const Track& track, double fileToDevice,
                          const SeparatedTrack* separation, const float* stemGains)
{
    // The stretcher says how much it wants; give it that, within the chunk it
    // was told to expect. Zero means it is full and only needs draining, but a
    // small top-up is harmless and keeps the loop moving.
    const auto required = static_cast<int> (stretcher->getSamplesRequired());
    const auto count = juce::jlimit (1, stretchChunkSize, required > 0 ? required : 64);
    const auto numSamples = static_cast<juce::int64> (track.audio.getNumSamples());

    // The input is the track resampled to the device rate at the recorded
    // speed, so the stretcher sees audio at its own sample rate and at the
    // original pitch. Off either end the read is silence, which is what a
    // stretcher should be fed there.
    // The loop has to be honoured on the way into the stretcher as well as on
    // the audible head. This is the head that actually chooses the audio when
    // key lock is on, so a loop that only wrapped the other one would be
    // inaudible: the deck would show itself looping and play straight through.
    const float* mix[2] = { track.audio.getReadPointer (0), track.audio.getReadPointer (1) };

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* out = stretchInput.getWritePointer (ch);
        auto position = feedPosition;

        for (int i = 0; i < count; ++i)
        {
            // The gains are taken as they stand rather than ramped: this audio
            // is going into a stretcher that will smear it over its own window,
            // which is far longer than any ramp would be.
            out[i] = readSample (mix, separation, stemGains, ch, numSamples, position);
            position += fileToDevice;
            wrapIntoLoop (position, fileToDevice, track);
        }
    }

    // Advance the shared head the same way, one sample at a time, so it lands
    // exactly where the per-channel reads did.
    for (int i = 0; i < count; ++i)
    {
        feedPosition += fileToDevice;
        wrapIntoLoop (feedPosition, fileToDevice, track);
    }

    const float* inputs[2] = { stretchInput.getReadPointer (0), stretchInput.getReadPointer (1) };
    stretcher->process (inputs, static_cast<size_t> (count), false);
}

void Deck::renderStretched (const Track& track, juce::AudioBuffer<float>& destination,
                            double rate, double fileToDevice, double pitchScale,
                            const SeparatedTrack* separation, const float* stemGains)
{
    stretcher->setPitchScale (juce::jlimit (0.25, 4.0, pitchScale));

    // Output samples per input sample. Each output sample must consume `rate`
    // file samples, and each input sample is fileToDevice of them.
    stretcher->setTimeRatio (juce::jlimit (0.25, 4.0, fileToDevice / rate));

    auto rounds = stretchRoundsPerBlock;

    if (! stretchPrimed)
    {
        // Line the stretcher up with the audible position. Its first
        // getStartDelay() output samples are warm-up, so produce and discard
        // them here; from then on the output is the track from readPosition.
        stretcher->reset();
        feedPosition = readPosition;

        auto toDiscard = static_cast<int> (stretcher->getStartDelay());

        while (toDiscard > 0 && rounds-- > 0)
        {
            if (stretcher->available() <= 0)
            {
                feedStretcher (track, fileToDevice, separation, stemGains);
                continue;
            }

            const auto count = juce::jmin (stretcher->available(), toDiscard, stretchChunkSize);
            float* outputs[2] = { stretchDiscard.getWritePointer (0), stretchDiscard.getWritePointer (1) };
            toDiscard -= static_cast<int> (stretcher->retrieve (outputs, static_cast<size_t> (count)));
        }

        stretchPrimed = true;
    }

    const auto blockSize = destination.getNumSamples();
    auto written = 0;

    while (written < blockSize && rounds-- > 0)
    {
        if (stretcher->available() <= 0)
        {
            feedStretcher (track, fileToDevice, separation, stemGains);
            continue;
        }

        const auto count = juce::jmin (stretcher->available(), blockSize - written);
        float* outputs[2] = { destination.getWritePointer (0) + written,
                              destination.getWritePointer (1) + written };
        written += static_cast<int> (stretcher->retrieve (outputs, static_cast<size_t> (count)));
    }

    // If the rounds ran out the rest of the block is the silence it was cleared
    // to, which is a dropout, and a dropout beats a stalled audio thread.
}

} // namespace opendj
