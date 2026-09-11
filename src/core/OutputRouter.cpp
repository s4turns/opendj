/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/OutputRouter.h"

namespace opendj
{

namespace
{
    /** One output channel, or null when the device has not got that one or is
        not using it. Everything below goes through this, which is why nothing
        below has to check a channel index again. */
    float* channel (float* const* outputs, int numOutputChannels, int index) noexcept
    {
        return juce::isPositiveAndBelow (index, numOutputChannels) ? outputs[index] : nullptr;
    }

    void copyChannel (float* destination, const juce::AudioBuffer<float>& source,
                      int sourceChannel, int numSamples) noexcept
    {
        if (destination != nullptr && sourceChannel < source.getNumChannels())
            juce::FloatVectorOperations::copy (destination,
                                               source.getReadPointer (sourceChannel),
                                               numSamples);
    }

    /** Both sides of a stereo bus, halved, into one output. Halving rather than
        summing, so a mono fold of a loud stereo mix does not clip where the
        stereo version did not. */
    void foldToMono (float* destination, const juce::AudioBuffer<float>& source,
                     int numSamples) noexcept
    {
        if (destination == nullptr || source.getNumChannels() < 1)
            return;

        const auto* left = source.getReadPointer (0);
        const auto* right = source.getNumChannels() > 1 ? source.getReadPointer (1) : left;

        for (int i = 0; i < numSamples; ++i)
            destination[i] = (left[i] + right[i]) * 0.5f;
    }
}

//==============================================================================

void routeOutputs (const juce::AudioBuffer<float>& master,
                   const juce::AudioBuffer<float>& cue,
                   float* const* outputs,
                   int numOutputChannels,
                   int numSamples,
                   OutputMode mode) noexcept
{
    if (outputs == nullptr || numOutputChannels <= 0 || numSamples <= 0)
        return;

    numSamples = juce::jmin (numSamples, master.getNumSamples(), cue.getNumSamples());

    if (numSamples <= 0)
        return;

    if (mode == OutputMode::splitStereo)
    {
        // Deliberately only the first two outputs, whatever else the device
        // has: a split is a statement about a cable, not about the hardware.
        foldToMono (channel (outputs, numOutputChannels, 0), master, numSamples);
        foldToMono (channel (outputs, numOutputChannels, 1), cue, numSamples);
        return;
    }

    copyChannel (channel (outputs, numOutputChannels, 0), master, 0, numSamples);
    copyChannel (channel (outputs, numOutputChannels, 1), master, 1, numSamples);

    // With fewer than four outputs the cue bus has nowhere to go. Putting it
    // anywhere else would send the headphone feed to the room.
    if (numOutputChannels >= 4)
    {
        copyChannel (channel (outputs, numOutputChannels, 2), cue, 0, numSamples);
        copyChannel (channel (outputs, numOutputChannels, 3), cue, 1, numSamples);
    }
}

bool cueIsAudible (OutputMode mode, int numOutputChannels) noexcept
{
    return mode == OutputMode::splitStereo ? numOutputChannels >= 2
                                           : numOutputChannels >= 4;
}

juce::String toString (OutputMode mode)
{
    return mode == OutputMode::splitStereo ? "split_stereo" : "separate_pairs";
}

OutputMode outputModeFromString (const juce::String& name)
{
    return name == "split_stereo" ? OutputMode::splitStereo : OutputMode::separatePairs;
}

} // namespace opendj
