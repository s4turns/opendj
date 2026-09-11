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

    // The dead zone either side of centre, so that a knob resting a hair off
    // the middle is still audibly out of the way.
    constexpr float filterDeadZone = 0.02f;

    constexpr float filterLowestCutoff = 120.0f;
    constexpr float filterHighestCutoff = 8000.0f;
    constexpr float filterOpenLow = 22000.0f;   // a low pass this high is transparent
    constexpr float filterOpenHigh = 15.0f;     // and a high pass this low likewise

    // Long enough for two beats at 60 BPM, which is slower than anything anyone
    // will echo. The line is sized once in prepare and never again.
    constexpr double maxEchoSeconds = 4.0;

    constexpr double shortestEchoSeconds = 0.02;

    /** The knob turned up raises the wet level and the feedback together. Kept
        under one so the echo always dies away: a DJ mixer that could be left
        self-oscillating is a mixer that will be. */
    float echoFeedbackFor (float amount) noexcept
    {
        return juce::jlimit (0.0f, 0.85f, amount * 0.85f);
    }

    /** Cutoff for the low pass half of the knob: transparent from the centre up. */
    float lowPassCutoffFor (float position)
    {
        if (position >= 0.5f - filterDeadZone)
            return filterOpenLow;

        const auto amount = juce::jlimit (0.0f, 1.0f, (0.5f - filterDeadZone - position) / (0.5f - filterDeadZone));
        return filterOpenLow * std::pow (filterLowestCutoff / filterOpenLow, amount);
    }

    /** And the high pass half: transparent from the centre down. */
    float highPassCutoffFor (float position)
    {
        if (position <= 0.5f + filterDeadZone)
            return filterOpenHigh;

        const auto amount = juce::jlimit (0.0f, 1.0f, (position - 0.5f - filterDeadZone) / (0.5f - filterDeadZone));
        return filterOpenHigh * std::pow (filterHighestCutoff / filterOpenHigh, amount);
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

    filterLow.prepare (spec);
    filterHigh.prepare (spec);
    filterLow.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterHigh.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    // Just enough resonance to give the sweep a voice, and not so much that a
    // knob left at the end of its travel whistles.
    filterLow.setResonance (0.8f);
    filterHigh.setResonance (0.8f);

    const auto maxBlock = (int) juce::jmax ((juce::uint32) 1, spec.maximumBlockSize);
    shapedBuffer.setSize (2, maxBlock, false, true, true);
    reverbBuffer.setSize (2, maxBlock, false, true, true);

    reverb.setSampleRate (spec.sampleRate);

    // Fed at full wet into its own buffer; the dry path is the channel itself,
    // and how much of the wet is heard is a gain applied afterwards.
    juce::Reverb::Parameters params;
    params.roomSize = 0.72f;
    params.damping = 0.4f;
    params.width = 1.0f;
    params.wetLevel = 1.0f;
    params.dryLevel = 0.0f;
    params.freezeMode = 0.0f;
    reverb.setParameters (params);

    reverbWet.reset (spec.sampleRate, smoothingSeconds);

    echo.setMaximumDelayInSamples (juce::jmax (2, (int) (maxEchoSeconds * spec.sampleRate)));
    echo.prepare (spec);

    // The delay length glides over a quarter of a second: long enough to hear
    // as a sweep, short enough that changing division still feels immediate.
    echoDelaySamples.reset (spec.sampleRate, 0.25);
    echoDelaySamples.setCurrentAndTargetValue ((float) (0.5 * spec.sampleRate));
    echoWet.reset (spec.sampleRate, smoothingSeconds);
    echoFeedback.reset (spec.sampleRate, smoothingSeconds);

    filterLowCutoff.reset (spec.sampleRate, smoothingSeconds);
    filterHighCutoff.reset (spec.sampleRate, smoothingSeconds);
    filterLowCutoff.setCurrentAndTargetValue (filterOpenLow);
    filterHighCutoff.setCurrentAndTargetValue (filterOpenHigh);
    filterLow.setCutoffFrequency (filterOpenLow);
    filterHigh.setCutoffFrequency (filterOpenHigh);

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
    filterLow.reset();
    filterHigh.reset();
    echo.reset();
    reverb.reset();
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

    // A and B sit on the crossfader because that is what a crossfader is for.
    // C and D run through it, since a third deck is nearly always an addition
    // to the mix rather than one side of it. Any of the four can be reassigned.
    strips[0].crossfaderAssign.store (CrossfaderAssign::a, std::memory_order_relaxed);
    strips[1].crossfaderAssign.store (CrossfaderAssign::b, std::memory_order_relaxed);

    recalculateCrossfader();
}

//==============================================================================
// Message thread
//==============================================================================

void Mixer::setChannelFader (int channel, float normalised)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
    {
        strips[(size_t) channel].faderPosition.store (normalised, std::memory_order_relaxed);
        strips[(size_t) channel].targetFaderGain.store (faderToGain (normalised),
                                                        std::memory_order_relaxed);
    }
}

float Mixer::getChannelFader (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
        ? strips[(size_t) channel].faderPosition.load (std::memory_order_relaxed)
        : 0.0f;
}

void Mixer::setChannelEq (int channel, int band, float normalised)
{
    if (juce::isPositiveAndBelow (channel, numChannels) && juce::isPositiveAndBelow (band, 3))
    {
        strips[(size_t) channel].bandPosition[(size_t) band].store (normalised, std::memory_order_relaxed);
        strips[(size_t) channel].targetBandGain[(size_t) band].store (eqKnobToGain (normalised),
                                                                      std::memory_order_relaxed);
    }
}

void Mixer::setChannelFilter (int channel, float normalised)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].filterPosition.store (juce::jlimit (0.0f, 1.0f, normalised),
                                                       std::memory_order_relaxed);
}

void Mixer::setChannelEcho (int channel, float amount)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].echoAmount.store (juce::jlimit (0.0f, 1.0f, amount),
                                                   std::memory_order_relaxed);
}

float Mixer::getChannelEcho (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
        ? strips[(size_t) channel].echoAmount.load (std::memory_order_relaxed)
        : 0.0f;
}

void Mixer::setChannelEchoTime (int channel, double seconds)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].echoSeconds.store (
            juce::jlimit (shortestEchoSeconds, maxEchoSeconds, seconds), std::memory_order_relaxed);
}

double Mixer::getChannelEchoTime (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
        ? strips[(size_t) channel].echoSeconds.load (std::memory_order_relaxed)
        : 0.0;
}

void Mixer::setChannelReverb (int channel, float amount)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].reverbAmount.store (juce::jlimit (0.0f, 1.0f, amount),
                                                     std::memory_order_relaxed);
}

float Mixer::getChannelReverb (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
        ? strips[(size_t) channel].reverbAmount.load (std::memory_order_relaxed)
        : 0.0f;
}

float Mixer::getChannelFilter (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
        ? strips[(size_t) channel].filterPosition.load (std::memory_order_relaxed)
        : 0.5f;
}

float Mixer::getChannelEq (int channel, int band) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels) && juce::isPositiveAndBelow (band, 3)
        ? strips[(size_t) channel].bandPosition[(size_t) band].load (std::memory_order_relaxed)
        : 0.5f;
}

void Mixer::setChannelCue (int channel, bool shouldMonitor)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
        strips[(size_t) channel].cueEnabled.store (shouldMonitor, std::memory_order_relaxed);
}

void Mixer::toggleChannelCue (int channel)
{
    if (juce::isPositiveAndBelow (channel, numChannels))
    {
        auto& flag = strips[(size_t) channel].cueEnabled;
        flag.store (! flag.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

bool Mixer::isChannelCued (int channel) const
{
    return juce::isPositiveAndBelow (channel, numChannels)
        && strips[(size_t) channel].cueEnabled.load (std::memory_order_relaxed);
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

void Mixer::setChannelCrossfaderAssign (int channel, CrossfaderAssign assign)
{
    if (! juce::isPositiveAndBelow (channel, numChannels))
        return;

    strips[(size_t) channel].crossfaderAssign.store (assign, std::memory_order_relaxed);
    recalculateCrossfader();
}

Mixer::CrossfaderAssign Mixer::getChannelCrossfaderAssign (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numChannels)
         ? strips[(size_t) channel].crossfaderAssign.load (std::memory_order_relaxed)
         : CrossfaderAssign::thru;
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

    for (auto& strip : strips)
    {
        const auto gain = strip.crossfaderAssign.load (std::memory_order_relaxed);

        strip.targetCrossfaderGain.store (gain == CrossfaderAssign::a ? gainA
                                        : gain == CrossfaderAssign::b ? gainB
                                                                      : 1.0f,
                                          std::memory_order_relaxed);
    }
}

void Mixer::setMasterGain (float normalised)
{
    masterPosition.store (normalised, std::memory_order_relaxed);
    targetMasterGain.store (faderToGain (normalised), std::memory_order_relaxed);
}

void Mixer::setCueGain (float normalised)
{
    cuePosition.store (normalised, std::memory_order_relaxed);
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

        // A cutoff is swept, not stepped: setting it per sample would be
        // needless work, but jumping it per block would zipper.
        const auto filterPosition = strip.filterPosition.load (std::memory_order_relaxed);
        strip.filterLowCutoff.setTargetValue (lowPassCutoffFor (filterPosition));
        strip.filterHighCutoff.setTargetValue (highPassCutoffFor (filterPosition));

        const auto echoAmount = strip.echoAmount.load (std::memory_order_relaxed);
        strip.echoDelaySamples.setTargetValue (
            (float) (strip.echoSeconds.load (std::memory_order_relaxed) * currentSampleRate));
        strip.echoWet.setTargetValue (echoAmount);
        strip.echoFeedback.setTargetValue (echoFeedbackFor (echoAmount));

        strip.faderGain.setTargetValue (strip.targetFaderGain.load (std::memory_order_relaxed));
        strip.crossfaderGain.setTargetValue (strip.targetCrossfaderGain.load (std::memory_order_relaxed));

        strip.reverbWet.setTargetValue (strip.reverbAmount.load (std::memory_order_relaxed));

        const auto monitoring = strip.cueEnabled.load (std::memory_order_relaxed);

        // The channel is built into a buffer first, because the reverb works on
        // a block rather than a sample at a time. The fader and crossfader are
        // applied afterwards, on the way out.
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gLow  = strip.bandGain[0].getNextValue();
            const auto gMid  = strip.bandGain[1].getNextValue();
            const auto gHigh = strip.bandGain[2].getNextValue();

            strip.filterLow.setCutoffFrequency (strip.filterLowCutoff.getNextValue());
            strip.filterHigh.setCutoffFrequency (strip.filterHighCutoff.getNextValue());

            const auto delaySamples = strip.echoDelaySamples.getNextValue();
            const auto wet = strip.echoWet.getNextValue();
            const auto feedback = strip.echoFeedback.getNextValue();
            strip.echo.setDelay (delaySamples);

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto input = deckBuffer->getSample (ch, i);

                float low = 0.0f, aboveLow = 0.0f, mid = 0.0f, high = 0.0f;
                strip.lowSplit.processSample (ch, input, low, aboveLow);
                strip.highSplit.processSample (ch, aboveLow, mid, high);

                // The low band skipped the second crossover, so match its phase.
                low = strip.lowAllpass.processSample (ch, low);

                auto shaped = low * gLow + mid * gMid + high * gHigh;

                // Filter after the EQ, which is where a DJ mixer puts it.
                shaped = strip.filterHigh.processSample ((int) ch, strip.filterLow.processSample ((int) ch, shaped));

                // Echo last, so it repeats whatever the EQ and filter made,
                // which is what a send on a DJ mixer does. Feeding the delay
                // even at zero wet keeps the line warm, so turning the knob up
                // brings in repeats of what just played rather than silence
                // followed by a sudden burst.
                const auto delayed = strip.echo.popSample ((int) ch);
                strip.echo.pushSample ((int) ch, shaped + delayed * feedback);
                shaped += delayed * wet;

                strip.shapedBuffer.setSample (ch, i, shaped);
            }
        }

        // Reverb over the whole block, at full wet into its own buffer. It runs
        // every block whatever the knob says, so a tail already ringing when it
        // is turned down finishes rather than being cut off.
        for (int ch = 0; ch < 2; ++ch)
            strip.reverbBuffer.copyFrom (ch, 0, strip.shapedBuffer, ch, 0, numSamples);

        strip.reverb.processStereo (strip.reverbBuffer.getWritePointer (0),
                                    strip.reverbBuffer.getWritePointer (1),
                                    numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto gFader = strip.faderGain.getNextValue();
            const auto gCross = strip.crossfaderGain.getNextValue();
            const auto gReverb = strip.reverbWet.getNextValue();

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto shaped = strip.shapedBuffer.getSample (ch, i)
                                  + strip.reverbBuffer.getSample (ch, i) * gReverb;

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
