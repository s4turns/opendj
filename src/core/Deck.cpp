/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/Deck.h"

#include "analysis/TrackAnalyser.h"

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

    // Refuse absurd files rather than exhausting memory on a mistaken load.
    constexpr double maxTrackMinutes = 30.0;
}

Deck::Deck (int deckIndex, juce::AudioFormatManager& formatManagerToUse)
    : index (deckIndex), formatManager (formatManagerToUse)
{
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

bool Deck::loadFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return false;

    const auto seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

    if (seconds > maxTrackMinutes * 60.0)
        return false;

    auto track = std::make_unique<Track>();
    track->file = file;
    track->sampleRate = reader->sampleRate;
    track->title = file.getFileNameWithoutExtension();
    track->audio.setSize (2, static_cast<int> (reader->lengthInSamples));

    if (! reader->read (&track->audio, 0, static_cast<int> (reader->lengthInSamples), 0, true, true))
        return false;

    // A mono file reads into channel 0 only, so mirror it across.
    if (reader->numChannels == 1)
        track->audio.copyFrom (1, 0, track->audio, 0, 0, track->audio.getNumSamples());

    // Waveform peaks and the beat grid are built here, on whichever thread is
    // doing the loading, so the track is fully described the moment it appears.
    auto analysis = TrackAnalyser::analyse (track->audio, track->sampleRate);

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
}

void Deck::releaseResources()
{
    transportGain.setCurrentAndTargetValue (0.0f);
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
        return;
    }

    const auto numSamples = static_cast<juce::int64> (track->audio.getNumSamples());

    const auto wantPlay = playing.load (std::memory_order_relaxed);
    transportGain.setTargetValue (wantPlay ? 1.0f : 0.0f);

    // A seek that arrives mid fade-out waits for the fade to finish. Pressing
    // cue while playing should cut the audio where it is and only then send the
    // head back, rather than stamping a fragment of the cue point over the tail.
    const auto fadingOut = ! wantPlay && transportGain.getCurrentValue() > 0.0f;

    if (! fadingOut)
        if (const auto seek = pendingSeekSeconds.exchange (-1.0, std::memory_order_relaxed); seek >= 0.0)
            readPosition = seek * track->sampleRate;

    // Once stopped and fully faded, hold position and cost nothing.
    if (! wantPlay && transportGain.getCurrentValue() <= 0.0f)
    {
        positionSeconds.store (readPosition / track->sampleRate, std::memory_order_relaxed);
        peakLevel.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const auto rate = (track->sampleRate / deviceSampleRate) * tempoRatio.load (std::memory_order_relaxed);
    const auto trim = trimGain.load (std::memory_order_relaxed);
    const auto blockSize = destination.getNumSamples();
    const auto outChannels = juce::jmin (2, destination.getNumChannels());

    const float* source[2] = { track->audio.getReadPointer (0), track->audio.getReadPointer (1) };

    auto position = readPosition;
    auto peak = 0.0f;

    for (int i = 0; i < blockSize; ++i)
    {
        const auto gain = transportGain.getNextValue() * trim;

        if (position >= 0.0 && position < static_cast<double> (numSamples))
        {
            for (int ch = 0; ch < outChannels; ++ch)
            {
                const auto sample = interpolate (source[ch], numSamples, position) * gain;
                destination.setSample (ch, i, sample);
                peak = juce::jmax (peak, std::abs (sample));
            }
        }

        position += rate;
    }

    // Running off either end stops the deck rather than looping.
    if (position >= static_cast<double> (numSamples))
    {
        position = static_cast<double> (numSamples);
        playing.store (false, std::memory_order_relaxed);
    }
    else if (position < 0.0)
    {
        position = 0.0;
        playing.store (false, std::memory_order_relaxed);
    }

    readPosition = position;
    positionSeconds.store (position / track->sampleRate, std::memory_order_relaxed);
    peakLevel.store (juce::jmax (peak, peakLevel.load (std::memory_order_relaxed)),
                     std::memory_order_relaxed);
}

} // namespace opendj
