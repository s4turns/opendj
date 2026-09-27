/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>

namespace opendj
{

/** Effects on the whole mix: two slots, each holding one effect with its own
    wet level and two parameters.

    They sit on the master bus after the channels are summed and before the
    master gain and soft clip, so they hear exactly what the room is about to.
    The sampler is mixed in after the mixer and stays dry, which keeps a drop
    or an air horn clean over a washed-out mix.

    Echo and reverb are sends: switching a slot off stops feeding it but lets
    what is already in it ring out, which is how an echo out is played. The
    filter is an insert, so off simply crossfades back to the dry signal.

    Every slot remembers its settings per effect, so flicking from echo to
    reverb and back finds the echo as it was left.
*/
class MasterEffects
{
public:
    static constexpr int numSlots = 2;
    static constexpr int numParams = 2;

    enum class Type
    {
        echo,
        reverb,
        filter
    };

    static constexpr int numTypes = 3;

    static juce::String getTypeName (Type type);

    /** What a parameter knob does for an effect, short enough for a label. */
    static juce::String getParamName (Type type, int param);

    /** The echo lengths, in beats, that the first echo parameter snaps to. */
    static constexpr std::array<double, 6> echoDivisions { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    static int echoDivisionIndexFor (float normalised) noexcept;
    static float normalisedForEchoDivision (int index) noexcept;
    static juce::String echoDivisionName (int index);

    MasterEffects();

    //==========================================================================
    // Message thread
    //==========================================================================

    void setType (int slot, Type type);
    Type getType (int slot) const noexcept;
    void stepType (int slot);   ///< on to the next effect, wrapping round

    void setEnabled (int slot, bool shouldBeOn);
    bool isEnabled (int slot) const noexcept;
    void toggleEnabled (int slot);

    /** Wet level and parameters for the slot's current effect. */
    void setWet (int slot, float normalised);
    float getWet (int slot) const noexcept;
    void setParam (int slot, int param, float normalised);
    float getParam (int slot, int param) const noexcept;

    /** The same for a named effect, current or not. For saving and restoring. */
    void setWetFor (int slot, Type type, float normalised);
    float getWetFor (int slot, Type type) const noexcept;
    void setParamFor (int slot, Type type, int param, float normalised);
    float getParamFor (int slot, Type type, int param) const noexcept;

    /** How long one beat of the mix lasts. Set by the engine from the deck the
        room is hearing, since nothing here knows about tempo. */
    void setBeatSeconds (double seconds);
    double getBeatSeconds() const noexcept { return beatSeconds.load (std::memory_order_relaxed); }

    /** One echo repeat in the slot, in seconds: the beat times the division. */
    double getEchoSeconds (int slot) const noexcept;

    //==========================================================================
    // Audio thread
    //==========================================================================

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Runs both slots over the first numSamples of a stereo buffer, in place. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    struct Slot
    {
        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        // Written on the message thread.
        std::atomic<Type> requestedType { Type::echo };
        std::atomic<bool> enabled { false };
        std::atomic<float> wet[numTypes] { { 0.5f }, { 0.4f }, { 1.0f } };
        std::atomic<float> params[numTypes][numParams]
        {
            { { 0.6f }, { 0.5f } },     // one beat, moderate feedback
            { { 0.6f }, { 0.4f } },     // a medium room
            { { 0.5f }, { 0.3f } }      // open, a little resonance
        };

        // Audio thread only. A change of effect fades the old one out, swaps,
        // and fades the new one in, so it can never click.
        Type activeType = Type::echo;
        juce::SmoothedValue<float> typeGain;
        juce::SmoothedValue<float> send, wetGain;

        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> echo { 1 };
        juce::SmoothedValue<float> echoDelaySamples, echoFeedback;
        bool echoNeedsLength = true;

        juce::Reverb reverb;
        juce::Reverb::Parameters reverbParams;
        juce::AudioBuffer<float> scratch;

        juce::dsp::StateVariableTPTFilter<float> filterLow, filterHigh;
        juce::SmoothedValue<float> filterLowCutoff, filterHighCutoff, filterResonance;
    };

    void processSlot (Slot& slot, juce::AudioBuffer<float>& buffer, int start, int numSamples);

    std::array<Slot, numSlots> slots;
    std::atomic<double> beatSeconds { 0.5 };
    double sampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterEffects)
};

} // namespace opendj
