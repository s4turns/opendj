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
}

Deck::~Deck()
{
    activeTrack.store (nullptr, std::memory_order_release);
    owned.reset();
    retired.clear();
}

//==============================================================================
// Loading
//==============================================================================

bool Deck::loadFile (const juce::File& file, const KnownTrack* known)
{
    auto decoded = TrackDecoder::decode (formatManager, file);

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
                                         known->tempoConfidence, {})
        : TrackAnalyser::analyse (track->audio, track->sampleRate);

    publish (std::move (track));
    analysisData.store (std::move (analysis));
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
    analysisData.store (nullptr);
}

std::shared_ptr<const TrackAnalysis> Deck::getAnalysis() const
{
    return analysisData.load();
}

void Deck::cleanUp()
{
    const auto now = blocksProcessed.load (std::memory_order_relaxed);

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
    if (isLoaded())
        playing.store (true, std::memory_order_relaxed);
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
        cuePointSeconds.store (position, std::memory_order_relaxed);
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
        cue.store (getPositionSeconds(), std::memory_order_relaxed);
    else
        seekToSeconds (cue.load (std::memory_order_relaxed));
}

void Deck::clearHotCue (int slot)
{
    if (juce::isPositiveAndBelow (slot, numHotCues))
        hotCues[(size_t) slot].store (-1.0, std::memory_order_relaxed);
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

    if (track == nullptr)
    {
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
        }
    }

    // Once stopped and fully faded, hold position and cost nothing.
    if (! audible && transportGain.getCurrentValue() <= 0.0f)
    {
        pitchBend = 0.0;
        stretchPrimed = false;
        scratchTargetValid = false;
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

    const auto trim = trimGain.load (std::memory_order_relaxed);
    const auto outChannels = juce::jmin (2, destination.getNumChannels());

    const float* source[2] = { track->audio.getReadPointer (0), track->audio.getReadPointer (1) };

    // Key lock is bypassed on the platter, and if it would have nothing to do:
    // at exactly the recorded speed the stretcher is a delay line, and a delay
    // that comes and goes with the fader is worse than a pitch that does.
    const auto stretching = keyLock.load (std::memory_order_relaxed)
                         && ! scratching
                         && stretcher != nullptr
                         && outChannels == 2
                         && std::abs (rate - fileToDevice) > 1.0e-6;

    if (stretching)
        renderStretched (*track, destination, rate, fileToDevice);
    else
        stretchPrimed = false;

    auto position = readPosition;
    auto peak = 0.0f;

    for (int i = 0; i < blockSize; ++i)
    {
        const auto gain = transportGain.getNextValue() * trim;
        const auto inRange = position >= 0.0 && position < static_cast<double> (numSamples);

        for (int ch = 0; ch < outChannels; ++ch)
        {
            const auto sample = stretching
                ? destination.getSample (ch, i) * gain
                : (inRange ? interpolate (source[ch], numSamples, position) * gain : 0.0f);

            destination.setSample (ch, i, sample);
            peak = juce::jmax (peak, std::abs (sample));
        }

        position += rate;
    }

    // Running off either end stops playback rather than looping. Scratching
    // just stops at the edge, since the hand is still on the platter.
    if (position >= static_cast<double> (numSamples))
    {
        position = static_cast<double> (numSamples);

        if (! scratching)
            playing.store (false, std::memory_order_relaxed);
    }
    else if (position < 0.0)
    {
        position = 0.0;

        if (! scratching)
            playing.store (false, std::memory_order_relaxed);
    }

    readPosition = position;
    positionSeconds.store (position / track->sampleRate, std::memory_order_relaxed);
    peakLevel.store (juce::jmax (peak, peakLevel.load (std::memory_order_relaxed)),
                     std::memory_order_relaxed);
}

//==============================================================================
// Key lock
//==============================================================================

void Deck::feedStretcher (const Track& track, double fileToDevice)
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
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* out = stretchInput.getWritePointer (ch);
        auto position = feedPosition;

        for (int i = 0; i < count; ++i)
        {
            out[i] = interpolate (track.audio.getReadPointer (ch), numSamples, position);
            position += fileToDevice;
        }
    }

    feedPosition += count * fileToDevice;

    const float* inputs[2] = { stretchInput.getReadPointer (0), stretchInput.getReadPointer (1) };
    stretcher->process (inputs, static_cast<size_t> (count), false);
}

void Deck::renderStretched (const Track& track, juce::AudioBuffer<float>& destination,
                            double rate, double fileToDevice)
{
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
                feedStretcher (track, fileToDevice);
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
            feedStretcher (track, fileToDevice);
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
