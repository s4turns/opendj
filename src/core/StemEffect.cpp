/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/StemEffect.h"

#include "core/EffectCurves.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace opendj
{

using namespace effectCurves;

namespace
{
    // The fade either side of a change of target or effect, and of switching on
    // and off. Short enough to feel like a switch, long enough not to click.
    constexpr double changeFadeSeconds = 0.02;

    // A change of division sweeps, as it does on the channel echo.
    constexpr double delayGlideSeconds = 0.25;

    // The way out goes quiet between repeats, so a tail is only over once it has
    // been quiet for longer than any repeat can take to come round again.
    constexpr double tailHoldSeconds = 5.0;
    static_assert (tailHoldSeconds > StemEffect::longestDelaySeconds);

    constexpr float silenceThreshold = 1.0e-5f;

    // Nothing above the audio callback guards against denormals, and a repeat
    // decaying towards nothing passes through them. Far below anything audible.
    constexpr float flushBelow = 1.0e-15f;

    // Below this much of the knob the low pass fades out rather than opening any
    // further, because a filter wide open still shifts the phase a little, and
    // the knob at zero should be no effect at all.
    constexpr float lowPassFadeBelow = 0.05f;

    // The knob stops short of the bottom of the channel filter's sweep: this is
    // there to take the top off one part, and at 120 Hz it would start on the
    // body of a bass line.
    constexpr float lowPassKnobTravel = 0.9f;

    constexpr float reverbRoomSize = 0.8f;
    constexpr float reverbDamping = 0.5f;

    float lowPassCutoffForAmount (float amount, double sampleRate) noexcept
    {
        return lowPassCutoffFor (0.5f * (1.0f - lowPassKnobTravel * amount), sampleRate);
    }

    float moveTowards (float value, float target, float step) noexcept
    {
        return value < target ? juce::jmin (value + step, target)
                              : juce::jmax (value - step, target);
    }

    float flushed (float value) noexcept
    {
        return std::abs (value) < flushBelow ? 0.0f : value;
    }
}

/** A stereo delay line that forgets in no time at all.

    juce::dsp::DelayLine clears all of itself to forget, and four seconds of it
    is megabytes at the higher sample rates, cleared on the audio thread at every
    stop and every load. Here forgetting is a count: whatever was written before
    it is read as silence, and is written over as the line goes round. */
class ForgetfulDelayLine
{
public:
    void prepare (int maximumDelaySamples)
    {
        // One past the longest delay, for the sample the interpolation reads
        // beside it, and one more for the sample about to be written.
        size = maximumDelaySamples + 2;

        for (auto& channel : samples)
            channel.assign ((size_t) size, 0.0f);

        writeIndex = 0;
        forget();
    }

    void forget() noexcept
    {
        written = 0;
    }

    /** What went in `delay` ticks before the next push. */
    float read (int channel, float delay) const noexcept
    {
        const auto clamped = juce::jlimit (1.0f, (float) (size - 2), delay);
        const auto whole = (int) clamped;
        const auto nearer = at (channel, whole);
        return nearer + (clamped - (float) whole) * (at (channel, whole + 1) - nearer);
    }

    void push (float left, float right) noexcept
    {
        samples[0][(size_t) writeIndex] = left;
        samples[1][(size_t) writeIndex] = right;
        writeIndex = writeIndex + 1 < size ? writeIndex + 1 : 0;
        written = juce::jmin (written + 1, size);
    }

private:
    float at (int channel, int ago) const noexcept
    {
        if (ago > written)
            return 0.0f;

        const auto index = writeIndex - ago;
        return samples[(size_t) channel][(size_t) (index < 0 ? index + size : index)];
    }

    std::array<std::vector<float>, 2> samples;
    int size = 4;
    int writeIndex = 0;
    int written = 0;     // how far back anything is remembered
};

struct StemEffect::Processors
{
    ForgetfulDelayLine line;
    juce::Reverb reverb;
    juce::dsp::StateVariableTPTFilter<float> filter;
};

StemEffect::StemEffect() : processors (std::make_unique<Processors>()) {}
StemEffect::~StemEffect() = default;

juce::String StemEffect::getTypeName (Type type)
{
    switch (type)
    {
        case Type::echo:    return "Echo";
        case Type::reverb:  return "Reverb";
        case Type::lowPass: return "Low pass";
    }

    return {};
}

juce::String StemEffect::getDivisionName (int division)
{
    static const char* const names[numDivisions] { "1/4", "1/2", "1", "2" };
    return names[juce::jlimit (0, numDivisions - 1, division)];
}

double StemEffect::delaySecondsFor (double beatSeconds, int division, double tempoRatio, bool stretching) noexcept
{
    const auto beats = beatDivisions[(size_t) juce::jlimit (0, numDivisions - 1, division)];
    const auto atTempo = stretching ? 1.0 : 1.0 / juce::jmax (0.001, tempoRatio);

    return juce::jlimit (shortestEchoSeconds, longestDelaySeconds, beatSeconds * beats * atTempo);
}

void StemEffect::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    fadeStep = static_cast<float> (1.0 / (changeFadeSeconds * sampleRate));
    tailHoldSamples = static_cast<juce::int64> (tailHoldSeconds * sampleRate);

    const juce::dsp::ProcessSpec spec { sampleRate, 512, 2 };
    auto& [line, reverb, filter] = *processors;

    line.prepare (static_cast<int> (std::ceil (longestDelaySeconds * sampleRate)) + 1);

    reverb.setSampleRate (sampleRate);

    juce::Reverb::Parameters parameters;
    parameters.roomSize = reverbRoomSize;
    parameters.damping = reverbDamping;
    parameters.width = 1.0f;
    parameters.wetLevel = 1.0f;
    parameters.dryLevel = 0.0f;
    parameters.freezeMode = 0.0f;
    reverb.setParameters (parameters);

    filter.prepare (spec);
    filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

    delaySamples.reset (sampleRate, delayGlideSeconds);
    amountRamp.reset (sampleRate, smoothingSeconds);
    cutoffRamp.reset (sampleRate, smoothingSeconds);

    reset();
}

void StemEffect::clearStateOf (Type type)
{
    switch (type)
    {
        case Type::echo:    processors->line.forget();  break;
        case Type::reverb:  processors->reverb.reset(); break;
        case Type::lowPass: processors->filter.reset(); break;
    }
}

void StemEffect::reset()
{
    // Only what is in use. The reverb is a few hundred kilobytes to clear.
    if (ringing)
        clearStateOf (activeType);

    ringing = false;
    send = 0.0f;
    output = 1.0f;
    silentFor = 0;
}

bool StemEffect::beginBlock (const Settings& settings) noexcept
{
    wanted = settings;

    const auto targetDelay = static_cast<float> (settings.delaySeconds * sampleRate);
    const auto targetCutoff = lowPassCutoffForAmount (settings.amount, sampleRate);

    // A fresh start takes what it is given. Only a change while running glides.
    if (! ringing)
    {
        ringing = true;
        activeType = settings.type;
        activeStem = settings.stem;

        delaySamples.setCurrentAndTargetValue (targetDelay);
        amountRamp.setCurrentAndTargetValue (settings.amount);
        cutoffRamp.setCurrentAndTargetValue (targetCutoff);
        processors->filter.setCutoffFrequency (targetCutoff);
        return true;
    }

    delaySamples.setTargetValue (targetDelay);
    amountRamp.setTargetValue (settings.amount);
    cutoffRamp.setTargetValue (targetCutoff);

    processors->filter.snapToZero();

    if (! settings.on && send <= 0.0f && (activeType == Type::lowPass || silentFor > tailHoldSamples))
    {
        reset();
        return false;
    }

    return true;
}

void StemEffect::tick (float inLeft, float inRight, float& deltaLeft, float& deltaRight) noexcept
{
    // A different stem comes in through the same door, so the door closes first.
    // A different effect also has to bring down what is coming out of this one.
    const auto typeChanging = wanted.type != activeType;
    const auto changing = typeChanging || wanted.stem != activeStem;

    send = moveTowards (send, wanted.on && ! changing ? 1.0f : 0.0f, fadeStep);
    output = moveTowards (output, typeChanging ? 0.0f : 1.0f, fadeStep);

    if (changing && send <= 0.0f && (! typeChanging || output <= 0.0f))
    {
        if (typeChanging)
            clearStateOf (activeType);

        activeType = wanted.type;
        activeStem = wanted.stem;
    }

    const auto amount = amountRamp.getNextValue();
    auto& [line, reverb, filter] = *processors;

    switch (activeType)
    {
        case Type::echo:
        {
            const auto feedback = echoFeedbackFor (amount);
            const auto wet = amount * output;

            const auto delay = delaySamples.getNextValue();
            const auto delayedLeft = line.read (0, delay);
            const auto delayedRight = line.read (1, delay);

            line.push (flushed (inLeft * send + delayedLeft * feedback),
                       flushed (inRight * send + delayedRight * feedback));

            deltaLeft = delayedLeft * wet;
            deltaRight = delayedRight * wet;
            break;
        }

        case Type::reverb:
        {
            auto left = inLeft * send;
            auto right = inRight * send;

            reverb.processStereo (&left, &right, 1);

            deltaLeft = left * amount * output;
            deltaRight = right * amount * output;
            break;
        }

        case Type::lowPass:
        {
            if (cutoffRamp.isSmoothing())
                filter.setCutoffFrequency (cutoffRamp.getNextValue());

            const auto wet = juce::jmin (1.0f, amount / lowPassFadeBelow) * send * output;

            deltaLeft = (filter.processSample (0, inLeft) - inLeft) * wet;
            deltaRight = (filter.processSample (1, inRight) - inRight) * wet;
            break;
        }
    }

    if (send > 0.0f || std::abs (deltaLeft) > silenceThreshold || std::abs (deltaRight) > silenceThreshold)
        silentFor = 0;
    else
        ++silentFor;
}

} // namespace opendj
