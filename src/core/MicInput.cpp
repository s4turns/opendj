/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/MicInput.h"

#include <cmath>

namespace opendj
{

namespace
{
    // Long enough that switching the mic on or off mid word does not click.
    constexpr double fadeSeconds = 0.01;

    // The music gets out of the way quickly and comes back slowly, so a pause
    // between sentences does not pump it up and down.
    constexpr double duckAttackSeconds = 0.05;
    constexpr double duckReleaseSeconds = 0.5;

    // A level knob on a controller moves in coarse steps.
    constexpr double gainSeconds = 0.02;

    /** The mixer rounds the master off above 0.7, and the voice is added after
        that, so it gets a round-off of its own. Higher, so nothing the mixer
        already let through is shaped a second time, and only applied where a
        voice is actually being added: a shout is rounded off rather than torn,
        and music with no voice on it is left exactly as the mixer made it. */
    inline float softClip (float x) noexcept
    {
        constexpr float threshold = 0.9f;
        constexpr float headroom = 1.0f - threshold;

        const auto magnitude = std::abs (x);

        if (magnitude <= threshold)
            return x;

        const auto shaped = threshold + headroom * std::tanh ((magnitude - threshold) / headroom);
        return x < 0.0f ? -shaped : shaped;
    }
}

//==============================================================================

MicInput::MicInput() { prepare (44100.0); }

void MicInput::prepare (double sampleRate)
{
    const auto rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto floor = (double) juce::Decibels::decibelsToGain (talkoverDecibels);

    envelopeStep = (float) (1.0 / (fadeSeconds * rate));
    duckDownStep = (float) ((1.0 - floor) / (duckAttackSeconds * rate));
    duckUpStep   = (float) ((1.0 - floor) / (duckReleaseSeconds * rate));
    gainStep     = (float) (2.0 / (gainSeconds * rate));

    // A device that restarts brings the mic back in with a fade, not a step.
    envelope = 0.0f;
    duckGain = 1.0f;
    smoothedGain = gain.load (std::memory_order_relaxed);
}

void MicInput::setGain (float newGain) noexcept
{
    gain.store (juce::jlimit (0.0f, 2.0f, newGain), std::memory_order_relaxed);
}

//==============================================================================

bool MicInput::processBlock (const float* const* inputs, int numInputChannels,
                             juce::AudioBuffer<float>& room,
                             juce::AudioBuffer<float>& recording,
                             int numSamples) noexcept
{
    if (numSamples <= 0 || room.getNumChannels() < 2 || room.getNumSamples() < numSamples)
        return false;

    const float* first = nullptr;
    const float* second = nullptr;

    if (inputs != nullptr)
    {
        for (int ch = 0; ch < numInputChannels && second == nullptr; ++ch)
        {
            if (inputs[ch] == nullptr)
                continue;

            if (first == nullptr)
                first = inputs[ch];
            else
                second = inputs[ch];
        }
    }

    const auto on = enabled.load (std::memory_order_relaxed);
    const auto targetGain = gain.load (std::memory_order_relaxed);
    const auto duckTarget = (on && talkover.load (std::memory_order_relaxed))
                          ? juce::Decibels::decibelsToGain (talkoverDecibels)
                          : 1.0f;
    const auto keepOutOfRoom = routing.load (std::memory_order_relaxed) == Routing::recordingOnly;

    // Off, faded out and with the music back at full level, nothing is touched.
    // The meter still runs, so a level can be set before the mic goes live.
    const auto idle = ! on && envelope <= 0.0f && duckGain >= 1.0f;

    const auto separate = ! idle && keepOutOfRoom
                       && recording.getNumChannels() >= 2 && recording.getNumSamples() >= numSamples;

    auto* roomL = room.getWritePointer (0);
    auto* roomR = room.getWritePointer (1);
    auto* recordingL = separate ? recording.getWritePointer (0) : nullptr;
    auto* recordingR = separate ? recording.getWritePointer (1) : nullptr;

    auto blockPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        auto voice = 0.0f;

        if (first != nullptr)  voice += first[i];
        if (second != nullptr) voice += second[i];

        smoothedGain = targetGain > smoothedGain ? juce::jmin (targetGain, smoothedGain + gainStep)
                                                 : juce::jmax (targetGain, smoothedGain - gainStep);
        voice *= smoothedGain;
        blockPeak = juce::jmax (blockPeak, std::abs (voice));

        if (idle)
            continue;

        envelope = on ? juce::jmin (1.0f, envelope + envelopeStep)
                      : juce::jmax (0.0f, envelope - envelopeStep);

        duckGain = duckTarget < duckGain ? juce::jmax (duckTarget, duckGain - duckDownStep)
                                         : juce::jmin (duckTarget, duckGain + duckUpStep);

        const auto left = roomL[i] * duckGain;
        const auto right = roomR[i] * duckGain;
        const auto spoken = voice * envelope;
        const auto speaking = spoken != 0.0f;

        const auto withVoiceL = speaking ? softClip (left + spoken) : left;
        const auto withVoiceR = speaking ? softClip (right + spoken) : right;

        if (separate)
        {
            recordingL[i] = withVoiceL;
            recordingR[i] = withVoiceR;
        }

        roomL[i] = keepOutOfRoom ? left : withVoiceL;
        roomR[i] = keepOutOfRoom ? right : withVoiceR;
    }

    // Kept as the loudest since the meter last looked, not only this block's,
    // so a peak between two repaints still shows.
    for (auto previous = peak.load (std::memory_order_relaxed);
         blockPeak > previous && ! peak.compare_exchange_weak (previous, blockPeak, std::memory_order_relaxed);)
    {
    }

    return separate;
}

} // namespace opendj
