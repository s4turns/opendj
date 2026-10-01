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
        case Type::plugin: return "Plugin";
    }

    return {};
}

juce::String MasterEffects::getParamName (Type type, int param)
{
    static const char* const names[numTypes][numParams]
    {
        { "Beats",  "Feedback" },
        { "Size",   "Damping" },
        { "Cutoff", "Resonance" },
        { "P1",     "P2" }
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

MasterEffects::~MasterEffects()
{
    // By now the audio callback has stopped, so every holder is ours.
    for (auto& slot : slots)
    {
        delete slot.incoming.exchange (nullptr);
        delete slot.outgoing.exchange (nullptr);
        delete slot.active;
    }
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
    auto next = (Type) (((int) getType (slot) + 1) % numTypes);

    if (next == Type::plugin && getPlugin (slot) == nullptr)
        next = (Type) (((int) next + 1) % numTypes);

    setType (slot, next);
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
    // A plugin's knob shows where its parameter really is, so a move made in
    // the plugin's own window is not undone by the knob the next time.
    if (type == Type::plugin && isValidSlot (slot) && isValidParam (param))
        if (auto* processor = getPlugin (slot))
            if (auto* parameter = processor->getParameters()[getPluginParamIndex (slot, param)])
                return parameter->getValue();

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

void MasterEffects::preparePlugin (juce::AudioProcessor& processor) const
{
    processor.setPlayConfigDetails (2, 2, sampleRate, preparedBlockSize);
    processor.prepareToPlay (sampleRate, preparedBlockSize);
}

bool MasterEffects::setPlugin (int slot, std::unique_ptr<juce::AudioProcessor> processor)
{
    if (! isValidSlot (slot))
        return false;

    auto& s = slots[(size_t) slot];

    if (processor != nullptr)
    {
        // Two channels is all the scratch buffer has. A plugin wanting a side
        // chain or a surround bus would be handed memory that is not there.
        if (processor->getTotalNumInputChannels() > 2 || processor->getTotalNumOutputChannels() > 2)
            return false;

        const juce::ScopedLock lock (prepareLock);
        preparePlugin (*processor);
    }

    auto* holder = new PluginHolder();
    holder->latency = processor != nullptr ? processor->getLatencySamples() : 0;
    holder->processor = std::move (processor);
    s.current = holder->processor.get();

    // Knobs pick up where the new plugin's parameters already are, rather
    // than dragging them to wherever the last plugin's were.
    for (int p = 0; p < numParams; ++p)
    {
        const auto value = getParamFor (slot, Type::plugin, p);
        s.params[indexOf (Type::plugin)][p].store (value, std::memory_order_relaxed);
        holder->knobsAtLoad[p] = value;
    }

    // One the audio thread never picked up is still ours to delete.
    delete s.incoming.exchange (holder, std::memory_order_acq_rel);
    return true;
}

juce::AudioProcessor* MasterEffects::getPlugin (int slot) const noexcept
{
    return isValidSlot (slot) ? slots[(size_t) slot].current : nullptr;
}

void MasterEffects::setPluginParamIndex (int slot, int param, int pluginParameterIndex)
{
    if (! isValidSlot (slot) || ! isValidParam (param))
        return;

    auto& s = slots[(size_t) slot];
    s.pluginParamIndex[param].store (juce::jmax (0, pluginParameterIndex), std::memory_order_relaxed);
    s.params[indexOf (Type::plugin)][param].store (getParamFor (slot, Type::plugin, param), std::memory_order_relaxed);
}

int MasterEffects::getPluginParamIndex (int slot, int param) const noexcept
{
    return isValidSlot (slot) && isValidParam (param)
        ? slots[(size_t) slot].pluginParamIndex[param].load (std::memory_order_relaxed)
        : 0;
}

juce::String MasterEffects::getParamLabel (int slot, int param) const
{
    const auto type = getType (slot);

    if (type == Type::plugin)
        if (auto* processor = getPlugin (slot))
            if (auto* parameter = processor->getParameters()[getPluginParamIndex (slot, param)])
                return parameter->getName (16);

    return getParamName (type, param);
}

void MasterEffects::collectGarbage()
{
    for (auto& slot : slots)
        delete slot.outgoing.exchange (nullptr, std::memory_order_acq_rel);
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

    dryDelay.setMaximumDelayInSamples (juce::jmax (2, (int) (0.5 * spec.sampleRate)));
    dryDelay.prepare (spec);

    midi.ensureSize (256);

    activeType = requestedType.load (std::memory_order_relaxed);
}

void MasterEffects::Slot::reset()
{
    echo.reset();
    echoNeedsLength = true;
    reverb.reset();
    filterLow.reset();
    filterHigh.reset();
    dryDelay.reset();
}

void MasterEffects::prepare (const juce::dsp::ProcessSpec& spec)
{
    const juce::ScopedLock lock (prepareLock);

    sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;
    preparedBlockSize = (int) juce::jmax ((juce::uint32) 1, spec.maximumBlockSize);

    for (auto& slot : slots)
    {
        slot.prepare (spec);
        slot.reset();

        // The device has changed under a loaded plugin, or one still waiting
        // to go in. The callback is stopped while this runs, so both are safe
        // to touch.
        for (auto* holder : { slot.active, slot.incoming.load (std::memory_order_acquire) })
            if (holder != nullptr && holder->processor != nullptr)
            {
                holder->processor->releaseResources();
                preparePlugin (*holder->processor);
                holder->latency = holder->processor->getLatencySamples();
            }

        slot.dryDelaySamples = slot.active != nullptr
            ? juce::jlimit (0, (int) slot.dryDelay.getMaximumDelayInSamples(), slot.active->latency)
            : 0;
        slot.dryDelay.setDelay ((float) slot.dryDelaySamples);
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

bool MasterEffects::swapPluginIfPossible (Slot& slot)
{
    // Waits while the last one handed back has not been collected: there is
    // nowhere to put a second, and freeing it here is not allowed.
    if (slot.incoming.load (std::memory_order_acquire) == nullptr
        || slot.outgoing.load (std::memory_order_acquire) != nullptr)
        return false;

    auto* next = slot.incoming.exchange (nullptr, std::memory_order_acq_rel);

    if (next == nullptr)
        return false;

    slot.outgoing.store (slot.active, std::memory_order_release);
    slot.active = next;

    // Only a knob that has moved since the plugin was put in is sent. Sending
    // both regardless would let one undo the other when they point at the
    // same parameter.
    for (int p = 0; p < numParams; ++p)
        slot.lastSentParam[p] = next->knobsAtLoad[p];

    slot.dryDelaySamples = juce::jlimit (0, (int) slot.dryDelay.getMaximumDelayInSamples(), next->latency);
    slot.dryDelay.reset();
    slot.dryDelay.setDelay ((float) slot.dryDelaySamples);
    return true;
}

void MasterEffects::processSlot (Slot& slot, juce::AudioBuffer<float>& buffer, int start, int numSamples)
{
    const auto requested = slot.requestedType.load (std::memory_order_relaxed);

    // A new plugin for a slot that is not playing one goes straight in, since
    // nothing of it is being heard.
    if (slot.activeType != Type::plugin)
        swapPluginIfPossible (slot);

    const auto pluginWaiting = slot.activeType == Type::plugin
                            && slot.incoming.load (std::memory_order_acquire) != nullptr;

    // A different effect asked for, or a different plugin: fade the current
    // one out, and only once it is silent swap it and fade the new one in from
    // a clean state.
    if (requested != slot.activeType || pluginWaiting)
    {
        if (slot.typeGain.getCurrentValue() <= 0.0f && ! slot.typeGain.isSmoothing())
        {
            swapPluginIfPossible (slot);

            // Still waiting for the old plugin to be collected: stay silent
            // rather than bring the old one back for a moment.
            if (requested == Type::plugin && slot.incoming.load (std::memory_order_acquire) != nullptr)
            {
                slot.typeGain.setTargetValue (0.0f);
            }
            else
            {
                slot.activeType = requested;
                slot.reset();
                slot.dryDelay.setDelay ((float) slot.dryDelaySamples);
                slot.typeGain.setTargetValue (1.0f);
            }
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

        case Type::plugin:
        {
            auto* processor = slot.active != nullptr ? slot.active->processor.get() : nullptr;

            // An empty plugin slot is a straight wire.
            if (processor == nullptr)
                break;

            // Knob moves reach the plugin here, on the thread it runs on,
            // whether they came from the screen or from a controller.
            const auto& parameters = processor->getParameters();

            for (int p = 0; p < numParams; ++p)
            {
                const auto value = p == 0 ? p0 : p1;

                if (value != slot.lastSentParam[p])
                {
                    if (auto* parameter = parameters[slot.pluginParamIndex[p].load (std::memory_order_relaxed)])
                        parameter->setValue (value);

                    slot.lastSentParam[p] = value;
                }
            }

            auto* wetL = slot.scratch.getWritePointer (0);
            auto* wetR = slot.scratch.getWritePointer (1);
            juce::FloatVectorOperations::copy (wetL, left, numSamples);
            juce::FloatVectorOperations::copy (wetR, right, numSamples);

            // A view of exactly this chunk, so the plugin never sees more
            // samples than it was prepared for. Referring to existing channels
            // allocates nothing.
            juce::AudioBuffer<float> view (slot.scratch.getArrayOfWritePointers(), 2, numSamples);
            slot.midi.clear();

            // The plugin's own lock, as every host takes it. A plugin busy
            // changing its setup, or suspended, is passed by for the block.
            {
                const juce::ScopedTryLock pluginLock (processor->getCallbackLock());

                if (! pluginLock.isLocked() || processor->isSuspended())
                    break;

                processor->processBlock (view, slot.midi);
            }

            const auto latent = slot.dryDelaySamples > 0;

            for (int i = 0; i < numSamples; ++i)
            {
                const auto mix = slot.wetGain.getNextValue() * slot.send.getNextValue()
                               * slot.typeGain.getNextValue();

                float* channels[] { left, right };
                const float* wet[] { wetL, wetR };

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto dry = channels[ch][i];

                    if (latent)
                    {
                        slot.dryDelay.pushSample (ch, dry);
                        dry = slot.dryDelay.popSample (ch);
                    }

                    channels[ch][i] = dry + (wet[ch][i] - dry) * mix;
                }
            }
            break;
        }
    }
}

} // namespace opendj
