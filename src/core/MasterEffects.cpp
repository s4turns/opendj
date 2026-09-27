/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/MasterEffects.h"

#include "core/EffectCurves.h"

namespace opendj
{

using namespace effectCurves;

namespace
{
    // The fade either side of a change of effect. Short enough to feel like a
    // switch, long enough that nothing is cut mid-cycle.
    constexpr double typeFadeSeconds = 0.02;

    bool isValidSlot (int slot) noexcept    { return juce::isPositiveAndBelow (slot, MasterEffects::numSlots); }
    bool isValidParam (int param) noexcept  { return juce::isPositiveAndBelow (param, MasterEffects::numParams); }

    size_t indexOf (MasterEffects::Type type) noexcept
    {
        return (size_t) juce::jlimit (0, MasterEffects::numTypes - 1, (int) type);
    }

    // A room from a cupboard to a hall. Never quite 1, where juce::Reverb stops
    // decaying at all.
    float roomSizeFor (float normalised) noexcept   { return 0.3f + 0.68f * juce::jlimit (0.0f, 1.0f, normalised); }

    // From a gentle slope to a sweep that sings, without reaching the whistle
    // a filter left at the end of its travel would otherwise make.
    float resonanceFor (float normalised) noexcept  { return 0.5f + 4.5f * juce::jlimit (0.0f, 1.0f, normalised); }
}

//==============================================================================

juce::String MasterEffects::getTypeName (Type type)
{
    switch (type)
    {
        case Type::echo:   return "Echo";
        case Type::reverb: return "Reverb";
        case Type::filter: return "Filter";
    }

    return {};
}

juce::String MasterEffects::getParamName (Type type, int param)
{
    static const char* const names[numTypes][numParams]
    {
        { "Beats",  "Feedback" },
        { "Size",   "Damping" },
        { "Cutoff", "Resonance" }
    };

    return isValidParam (param) ? names[indexOf (type)][param] : "";
}

int MasterEffects::echoDivisionIndexFor (float normalised) noexcept
{
    const auto last = (int) echoDivisions.size() - 1;
    return juce::jlimit (0, last, juce::roundToInt (juce::jlimit (0.0f, 1.0f, normalised) * (float) last));
}

float MasterEffects::normalisedForEchoDivision (int index) noexcept
{
    const auto last = (int) echoDivisions.size() - 1;
    return (float) juce::jlimit (0, last, index) / (float) last;
}

juce::String MasterEffects::echoDivisionName (int index)
{
    static const char* const names[] { "1/8", "1/4", "1/2", "1", "2", "4" };
    return names[juce::jlimit (0, (int) echoDivisions.size() - 1, index)];
}

//==============================================================================

MasterEffects::MasterEffects()
{
    // Two different effects to start with, so both slots are worth having
    // before anybody has touched a menu.
    slots[1].requestedType.store (Type::reverb, std::memory_order_relaxed);
    slots[1].activeType = Type::reverb;
}

void MasterEffects::setType (int slot, Type type)
{
    if (isValidSlot (slot))
        slots[(size_t) slot].requestedType.store ((Type) indexOf (type), std::memory_order_relaxed);
}

MasterEffects::Type MasterEffects::getType (int slot) const noexcept
{
    return isValidSlot (slot) ? slots[(size_t) slot].requestedType.load (std::memory_order_relaxed)
                              : Type::echo;
}

void MasterEffects::stepType (int slot)
{
    setType (slot, (Type) (((int) getType (slot) + 1) % numTypes));
}

void MasterEffects::setEnabled (int slot, bool shouldBeOn)
{
    if (isValidSlot (slot))
        slots[(size_t) slot].enabled.store (shouldBeOn, std::memory_order_relaxed);
}

bool MasterEffects::isEnabled (int slot) const noexcept
{
    return isValidSlot (slot) && slots[(size_t) slot].enabled.load (std::memory_order_relaxed);
}

void MasterEffects::toggleEnabled (int slot)
{
    setEnabled (slot, ! isEnabled (slot));
}

void MasterEffects::setWet (int slot, float normalised)                { setWetFor (slot, getType (slot), normalised); }
float MasterEffects::getWet (int slot) const noexcept                  { return getWetFor (slot, getType (slot)); }
void MasterEffects::setParam (int slot, int param, float normalised)   { setParamFor (slot, getType (slot), param, normalised); }
float MasterEffects::getParam (int slot, int param) const noexcept     { return getParamFor (slot, getType (slot), param); }

void MasterEffects::setWetFor (int slot, Type type, float normalised)
{
    if (isValidSlot (slot))
        slots[(size_t) slot].wet[indexOf (type)].store (juce::jlimit (0.0f, 1.0f, normalised),
                                                        std::memory_order_relaxed);
}

float MasterEffects::getWetFor (int slot, Type type) const noexcept
{
    return isValidSlot (slot) ? slots[(size_t) slot].wet[indexOf (type)].load (std::memory_order_relaxed)
                              : 0.0f;
}

void MasterEffects::setParamFor (int slot, Type type, int param, float normalised)
{
    if (isValidSlot (slot) && isValidParam (param))
        slots[(size_t) slot].params[indexOf (type)][param].store (juce::jlimit (0.0f, 1.0f, normalised),
                                                                  std::memory_order_relaxed);
}

float MasterEffects::getParamFor (int slot, Type type, int param) const noexcept
{
    return isValidSlot (slot) && isValidParam (param)
        ? slots[(size_t) slot].params[indexOf (type)][param].load (std::memory_order_relaxed)
        : 0.0f;
}

void MasterEffects::setBeatSeconds (double seconds)
{
    beatSeconds.store (seconds > 0.0 ? seconds : 0.5, std::memory_order_relaxed);
}

double MasterEffects::getEchoSeconds (int slot) const noexcept
{
    const auto division = echoDivisions[(size_t) echoDivisionIndexFor (getParamFor (slot, Type::echo, 0))];
    return juce::jlimit (shortestEchoSeconds, maxEchoSeconds, getBeatSeconds() * division);
}

//==============================================================================
// Audio thread
//==============================================================================

void MasterEffects::Slot::prepare (const juce::dsp::ProcessSpec& spec)
{
    const auto maxBlock = (int) juce::jmax ((juce::uint32) 1, spec.maximumBlockSize);
    scratch.setSize (2, maxBlock, false, true, true);

    echo.setMaximumDelayInSamples (juce::jmax (2, (int) (maxEchoSeconds * spec.sampleRate)));
    echo.prepare (spec);

    // The same glide as the channel echo: a change of division sweeps.
    echoDelaySamples.reset (spec.sampleRate, 0.25);
    echoDelaySamples.setCurrentAndTargetValue ((float) (0.5 * spec.sampleRate));
    echoFeedback.reset (spec.sampleRate, smoothingSeconds);

    reverb.setSampleRate (spec.sampleRate);
    reverbParams.width = 1.0f;
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverbParams.freezeMode = 0.0f;
    reverb.setParameters (reverbParams);

    filterLow.prepare (spec);
    filterHigh.prepare (spec);
    filterLow.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterHigh.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    filterLowCutoff.reset (spec.sampleRate, smoothingSeconds);
    filterHighCutoff.reset (spec.sampleRate, smoothingSeconds);
    filterResonance.reset (spec.sampleRate, smoothingSeconds);
    filterLowCutoff.setCurrentAndTargetValue (filterOpenLow);
    filterHighCutoff.setCurrentAndTargetValue (filterOpenHigh);
    filterLow.setCutoffFrequency (filterOpenLow);
    filterHigh.setCutoffFrequency (filterOpenHigh);

    typeGain.reset (spec.sampleRate, typeFadeSeconds);
    typeGain.setCurrentAndTargetValue (1.0f);
    send.reset (spec.sampleRate, smoothingSeconds);
    wetGain.reset (spec.sampleRate, smoothingSeconds);

    activeType = requestedType.load (std::memory_order_relaxed);
}

void MasterEffects::Slot::reset()
{
    echo.reset();
    echoNeedsLength = true;
    reverb.reset();
    filterLow.reset();
    filterHigh.reset();
}

void MasterEffects::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

    for (auto& slot : slots)
    {
        slot.prepare (spec);
        slot.reset();
    }
}

void MasterEffects::reset()
{
    for (auto& slot : slots)
        slot.reset();
}

void MasterEffects::process (juce::AudioBuffer<float>& buffer, int numSamples)
{
    numSamples = juce::jmin (numSamples, buffer.getNumSamples());

    if (numSamples <= 0 || buffer.getNumChannels() < 2)
        return;

    // A device handing over a block longer than prepare() promised is worked
    // through in pieces the scratch buffers can hold, rather than read past.
    const auto chunk = slots[0].scratch.getNumSamples();

    for (int start = 0; start < numSamples; start += chunk)
        for (auto& slot : slots)
            processSlot (slot, buffer, start, juce::jmin (chunk, numSamples - start));
}

void MasterEffects::processSlot (Slot& slot, juce::AudioBuffer<float>& buffer, int start, int numSamples)
{
    // A different effect asked for: fade the current one out, and only once it
    // is silent swap it and fade the new one in from a clean state.
    const auto requested = slot.requestedType.load (std::memory_order_relaxed);

    if (requested != slot.activeType)
    {
        if (slot.typeGain.getCurrentValue() <= 0.0f && ! slot.typeGain.isSmoothing())
        {
            slot.activeType = requested;
            slot.reset();
            slot.typeGain.setTargetValue (1.0f);
        }
        else
        {
            slot.typeGain.setTargetValue (0.0f);
        }
    }
    else
    {
        slot.typeGain.setTargetValue (1.0f);
    }

    const auto type = indexOf (slot.activeType);
    const auto p0 = slot.params[type][0].load (std::memory_order_relaxed);
    const auto p1 = slot.params[type][1].load (std::memory_order_relaxed);

    slot.send.setTargetValue (slot.enabled.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    slot.wetGain.setTargetValue (slot.wet[type].load (std::memory_order_relaxed));

    auto* left  = buffer.getWritePointer (0, start);
    auto* right = buffer.getWritePointer (1, start);

    switch (slot.activeType)
    {
        case Type::echo:
        {
            const auto division = echoDivisions[(size_t) echoDivisionIndexFor (p0)];
            const auto seconds = juce::jlimit (shortestEchoSeconds, maxEchoSeconds, getBeatSeconds() * division);
            // A fresh echo starts at its length; only a change while running
            // is swept, or the first repeats would slide in from nowhere.
            if (slot.echoNeedsLength)
                slot.echoDelaySamples.setCurrentAndTargetValue ((float) (seconds * sampleRate));
            else
                slot.echoDelaySamples.setTargetValue ((float) (seconds * sampleRate));

            slot.echoNeedsLength = false;
            slot.echoFeedback.setTargetValue (echoFeedbackFor (p1));

            for (int i = 0; i < numSamples; ++i)
            {
                const auto s = slot.send.getNextValue();
                const auto out = slot.wetGain.getNextValue() * slot.typeGain.getNextValue();
                const auto feedback = slot.echoFeedback.getNextValue();
                slot.echo.setDelay (slot.echoDelaySamples.getNextValue());

                float* channels[] { left, right };

                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto dry = channels[ch][i];
                    const auto delayed = slot.echo.popSample (ch);
                    slot.echo.pushSample (ch, dry * s + delayed * feedback);
                    channels[ch][i] = dry + delayed * out;
                }
            }
            break;
        }

        case Type::reverb:
        {
            const auto roomSize = roomSizeFor (p0);
            const auto damping = juce::jlimit (0.0f, 1.0f, p1);

            if (roomSize != slot.reverbParams.roomSize || damping != slot.reverbParams.damping)
            {
                slot.reverbParams.roomSize = roomSize;
                slot.reverbParams.damping = damping;
                slot.reverb.setParameters (slot.reverbParams);
            }

            auto* sendL = slot.scratch.getWritePointer (0);
            auto* sendR = slot.scratch.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto s = slot.send.getNextValue();
                sendL[i] = left[i] * s;
                sendR[i] = right[i] * s;
            }

            // Fed on whether or not the slot is on, so a tail already ringing
            // when it is switched off rings out instead of stopping dead.
            slot.reverb.processStereo (sendL, sendR, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto out = slot.wetGain.getNextValue() * slot.typeGain.getNextValue();
                left[i]  += sendL[i] * out;
                right[i] += sendR[i] * out;
            }
            break;
        }

        case Type::filter:
        {
            slot.filterLowCutoff.setTargetValue (lowPassCutoffFor (p0));
            slot.filterHighCutoff.setTargetValue (highPassCutoffFor (p0));
            slot.filterResonance.setTargetValue (resonanceFor (p1));

            for (int i = 0; i < numSamples; ++i)
            {
                slot.filterLow.setCutoffFrequency (slot.filterLowCutoff.getNextValue());
                slot.filterHigh.setCutoffFrequency (slot.filterHighCutoff.getNextValue());

                if (slot.filterResonance.isSmoothing() || i == 0)
                {
                    const auto q = slot.filterResonance.getNextValue();
                    slot.filterLow.setResonance (q);
                    slot.filterHigh.setResonance (q);
                }

                // An insert rather than a send: off, dry and wet all come down
                // to how far the output leans from the dry signal to the filtered.
                const auto mix = slot.wetGain.getNextValue() * slot.send.getNextValue()
                               * slot.typeGain.getNextValue();

                float* channels[] { left, right };

                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto dry = channels[ch][i];
                    const auto filtered = slot.filterHigh.processSample (ch, slot.filterLow.processSample (ch, dry));
                    channels[ch][i] = dry + (filtered - dry) * mix;
                }
            }
            break;
        }
    }
}

} // namespace opendj
