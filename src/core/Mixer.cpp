/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/Mixer.h"

#include <cmath>

namespace opendj
{

namespace
{
    constexpr double smoothingSeconds = 0.02;

    /** Maps a 0 to 1 EQ knob onto a gain, with 0.5 flat, 0 a full kill and 1
        about +6 dB. The lower half is squared so the last part of the sweep
        into kill feels gradual rather than falling off a cliff. */
    float eqKnobToGain (float normalised) noexcept
    {
        normalised = juce::jlimit (0.0f, 1.0f, normalised);

        if (normalised <= 0.5f)
        {
            const auto t = normalised * 2.0f;
            return t * t;
        }

        const auto t = (normalised - 0.5f) * 2.0f;
        return 1.0f + t;
    }

    /** Leaves everything below the threshold untouched and rounds off only what
        is above it. A plain tanh would start compressing at ordinary mixing
        levels, which would quietly cost the mixer its unity gain. */
    float softClip (float x) noexcept
    {
        constexpr float threshold = 0.7f;
        constexpr float headroom = 1.0f - threshold;

        const auto magnitude = std::abs (x);

        if (magnitude <= threshold)
            return x;

        const auto excess = (magnitude - threshold) / headroom;
        const auto shaped = threshold + headroom * std::tanh (excess);

        return x < 0.0f ? -shaped : shaped;
    }

    /** Faders follow a gentle curve rather than raw linear, which puts useful
        resolution in the top of the throw where mixing actually happens. */
    float faderToGain (float normalised) noexcept
    {
        normalised = juce::jlimit (0.0f, 1.0f, normalised);
        return normalised * normalised;
    }
}

//==============================================================================

void Mixer::ChannelStrip::prepare (const juce::dsp::ProcessSpec& spec)
{
    lowSplit.prepare (spec);
    highSplit.prepare (spec);
    lowAllpass.prepare (spec);

    lowSplit.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    highSplit.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    lowAllpass.setType (juce::dsp::LinkwitzRileyFilterType::allpass);

    for (auto& g : bandGain)
        g.reset (spec.sampleRate, smoothingSeconds);

    faderGain.reset (spec.sampleRate, smoothingSeconds);
    crossfaderGain.reset (spec.sampleRate, smoothingSeconds);
}

void Mixer::ChannelStrip::reset()
{
    lowSplit.reset();
    highSplit.reset();
    lowAllpass.reset();
}

//==============================================================================

Mixer::Mixer()
{
    for (auto& strip : strips)
    {
        strip.targetFaderGain.store (faderToGain (0.8f), std::memory_order_relaxed);

        for (auto& g : strip.targetBandGain)
            g.store (1.0f, std::memory_order_relaxed);
    }

    recalculateCrossfader();
}

//==============================================================================
// Message thread
//==============================================================================

void Mixer::setChannelFader (int channel, float normalised)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].targetFaderGain.store (faderToGain (normalised),
                                                        std::memory_order_relaxed);
}

void Mixer::setChannelEq (int channel, int band, float normalised)
{
    if (juce::isPositiveAndBelow (channel, numChannels) && juce::isPositiveAndBelow (band, 3))
        strips[(size_t) channel].targetBandGain[(size_t) band].store (eqKnobToGain (normalised),
                                                                      std::memory_order_relaxed);
}

void Mixer::setChannelCue (int channel, bool shouldMonitor)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].cueEnabled.store (shouldMonitor, std::memory_order_relaxed);
}

void Mixer::setCrossfaderPosition (float position)
{
    crossfaderPosition.store (juce::jlimit (-1.0f, 1.0f, position), std::memory_order_relaxed);
    recalculateCrossfader();
}

void Mixer::setCrossfaderCurve (CrossfaderCurve newCurve)
{
    curve.store (newCurve, std::memory_order_relaxed);
    recalculateCrossfader();
}

void Mixer::recalculateCrossfader()
{
    // Position runs -1 to +1; x runs 0 (hard A) to 1 (hard B).
    const auto x = (crossfaderPosition.load (std::memory_order_relaxed) + 1.0f) * 0.5f;

    float gainA = 1.0f;
    float gainB = 1.0f;

    switch (curve.load (std::memory_order_relaxed))
    {
        case CrossfaderCurve::constantPower:
            gainA = std::cos (x * juce::MathConstants<float>::halfPi);
            gainB = std::sin (x * juce::MathConstants<float>::halfPi);
            break;

        case CrossfaderCurve::linear:
            gainA = 1.0f - x;
            gainB = x;
            break;

        case CrossfaderCurve::sharpCut:
            // Full level until the very end of the throw, which is what makes
            // cutting and transform tricks possible.
            gainA = juce::jlimit (0.0f, 1.0f, (1.0f - x) * 8.0f);
            gainB = juce::jlimit (0.0f, 1.0f, x * 8.0f);
            break;
    }

    strips[0].targetCrossfaderGain.store (gainA, std::memory_order_relaxed);
    strips[1].targetCrossfaderGain.store (gainB, std::memory_order_relaxed);
}

void Mixer::setMasterGain (float normalised)
{
    targetMasterGain.store (faderToGain (normalised), std::memory_order_relaxed);
}

void Mixer::setCueGain (float normalised)
{
    targetCueGain.store (faderToGain (normalised), std::memory_order_relaxed);
}

void Mixer::setCueMix (float normalised)
{
    targetCueMix.store (juce::jlimit (0.0f, 1.0f, normalised), std::memory_order_relaxed);
}

float Mixer::getMasterPeak (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, 2)
        ? masterPeak[channel].load (std::memory_order_relaxed)
        : 0.0f;
}

//==============================================================================
// Audio thread
//==============================================================================

void Mixer::prepare (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (juce::jmax (1, blockSize));
    spec.numChannels = 2;

    for (auto& strip : strips)
    {
        strip.prepare (spec);
        strip.lowSplit.setCutoffFrequency (lowCrossoverHz);
        strip.highSplit.setCutoffFrequency (highCrossoverHz);
        strip.lowAllpass.setCutoffFrequency (highCrossoverHz);
    }

    masterGain.reset (currentSampleRate, smoothingSeconds);
    cueGain.reset (currentSampleRate, smoothingSeconds);
    cueMix.reset (currentSampleRate, smoothingSeconds);

    reset();
}

void Mixer::reset()
{
    for (auto& strip : strips)
        strip.reset();
}

void Mixer::processBlock (const std::array<juce::AudioBuffer<float>*, numChannels>& deckBuffers,
                          juce::AudioBuffer<float>& master,
                          juce::AudioBuffer<float>& cue)
{
    master.clear();
    cue.clear();

    const auto numSamples = master.getNumSamples();

    masterGain.setTargetValue (targetMasterGain.load (std::memory_order_relaxed));
    cueGain.setTargetValue (targetCueGain.load (std::memory_order_relaxed));
    cueMix.setTargetValue (targetCueMix.load (std::memory_order_relaxed));

    for (size_t c = 0; c < (size_t) numChannels; ++c)
    {
        auto& strip = strips[c];
        auto* deckBuffer = deckBuffers[c];

        if (deckBuffer == nullptr)
            continue;

        for (size_t b = 0; b < 3; ++b)
            strip.bandGain[b].setTargetValue (strip.targetBandGain[b].load (std::memory_order_relaxed));

        strip.faderGain.setTargetValue (strip.targetFaderGain.load (std::memory_order_relaxed));
        strip.crossfaderGain.setTargetValue (strip.targetCrossfaderGain.load (std::memory_order_relaxed));

        const auto monitoring = strip.cueEnabled.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto gLow  = strip.bandGain[0].getNextValue();
            const auto gMid  = strip.bandGain[1].getNextValue();
            const auto gHigh = strip.bandGain[2].getNextValue();
            const auto gFader = strip.faderGain.getNextValue();
            const auto gCross = strip.crossfaderGain.getNextValue();

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto input = deckBuffer->getSample (ch, i);

                float low = 0.0f, aboveLow = 0.0f, mid = 0.0f, high = 0.0f;
                strip.lowSplit.processSample (ch, input, low, aboveLow);
                strip.highSplit.processSample (ch, aboveLow, mid, high);

                // The low band skipped the second crossover, so match its phase.
                low = strip.lowAllpass.processSample (ch, low);

                const auto shaped = low * gLow + mid * gMid + high * gHigh;

                // The cue bus is pre-fader and pre-crossfader, so a track can be
                // lined up in headphones before it is brought into the mix.
                if (monitoring)
                    cue.addSample (ch, i, shaped);

                master.addSample (ch, i, shaped * gFader * gCross);
            }
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const auto gMaster = masterGain.getNextValue();
        const auto gCue = cueGain.getNextValue();
        const auto mix = cueMix.getNextValue();

        for (int ch = 0; ch < 2; ++ch)
        {
            auto masterSample = master.getSample (ch, i) * gMaster;

            // A soft clip rather than a hard one: pushed levels distort the way
            // an analogue desk does instead of tearing.
            masterSample = softClip (masterSample);
            master.setSample (ch, i, masterSample);

            // The cue knob blends between the monitored channels and the master,
            // so you can check a transition against what the room is hearing.
            const auto blended = cue.getSample (ch, i) * (1.0f - mix) + masterSample * mix;
            cue.setSample (ch, i, softClip (blended * gCue));
        }
    }

    for (int ch = 0; ch < 2; ++ch)
        masterPeak[ch].store (master.getMagnitude (ch, 0, numSamples), std::memory_order_relaxed);
}

} // namespace opendj
