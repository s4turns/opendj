/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
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

    A slot can also hold a plugin: any stereo AudioProcessor, which in the
    application is a VST3 effect. It is an insert like the filter, its two
    parameter knobs drive whichever of its parameters they are pointed at,
    and it is handed to the audio thread and back without the audio thread
    ever allocating or freeing anything.
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
        filter,
        plugin
    };

    static constexpr int numTypes = 4;

    static juce::String getTypeName (Type type);

    /** What a parameter knob does for an effect, short enough for a label.
        A plugin's knobs are named by getParamLabel, which knows the plugin. */
    static juce::String getParamName (Type type, int param);

    /** The echo lengths, in beats, that the first echo parameter snaps to. */
    static constexpr std::array<double, 6> echoDivisions { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    static int echoDivisionIndexFor (float normalised) noexcept;
    static float normalisedForEchoDivision (int index) noexcept;
    static juce::String echoDivisionName (int index);

    MasterEffects();
    ~MasterEffects();

    //==========================================================================
    // Message thread
    //==========================================================================

    void setType (int slot, Type type);
    Type getType (int slot) const noexcept;

    /** On to the next effect, wrapping round. The plugin is skipped when the
        slot has none, since stepping onto it would only be a dead stop. */
    void stepType (int slot);

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

    /** Puts a plugin in the slot, or takes it out with nullptr. It is prepared
        here, at the rate and block size the slot was last prepared with, and
        then handed over; the one it replaces comes back through
        collectGarbage. Returns false, and keeps the old one, for a processor
        wanting more than two channels in or out. */
    bool setPlugin (int slot, std::unique_ptr<juce::AudioProcessor> processor);

    /** The plugin most recently put in the slot, for its editor, its state and
        its parameter names. Owned here; valid until it is replaced. */
    juce::AudioProcessor* getPlugin (int slot) const noexcept;

    /** Which of the plugin's parameters a knob drives. Pointing a knob at a
        parameter picks up where that parameter already is. */
    void setPluginParamIndex (int slot, int param, int pluginParameterIndex);
    int getPluginParamIndex (int slot, int param) const noexcept;

    /** A knob's label: the effect's own name for it, or the plugin's. */
    juce::String getParamLabel (int slot, int param) const;

    /** Deletes plugins the audio thread has finished with. Call it now and
        then from the message thread; the engine's timer does. */
    void collectGarbage();

    //==========================================================================
    // Audio thread
    //==========================================================================

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    /** Runs both slots over the first numSamples of a stereo buffer, in place. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    /** A plugin on its way to or from the audio thread. Null inside means an
        empty slot, which is how a plugin is taken out again. */
    struct PluginHolder
    {
        std::unique_ptr<juce::AudioProcessor> processor;
        int latency = 0;

        // Where the knobs stood when it was put in, which is where its
        // parameters already are. Only a knob that moves on from here is sent.
        float knobsAtLoad[numParams] { -1.0f, -1.0f };
    };

    struct Slot
    {
        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        // Written on the message thread.
        std::atomic<Type> requestedType { Type::echo };
        std::atomic<bool> enabled { false };
        std::atomic<float> wet[numTypes] { { 0.5f }, { 0.4f }, { 1.0f }, { 1.0f } };
        std::atomic<float> params[numTypes][numParams]
        {
            { { 0.6f }, { 0.5f } },     // one beat, moderate feedback
            { { 0.6f }, { 0.4f } },     // a medium room
            { { 0.5f }, { 0.3f } },     // open, a little resonance
            { { 0.5f }, { 0.5f } }      // replaced by the plugin's own values
        };
        std::atomic<int> pluginParamIndex[numParams] { { 0 }, { 1 } };

        // A plugin goes in through `incoming` and its predecessor comes out
        // through `outgoing`, each taken with an exchange, so exactly one side
        // owns a holder at any moment. `current` is the message thread's view
        // of the newest one.
        std::atomic<PluginHolder*> incoming { nullptr }, outgoing { nullptr };
        juce::AudioProcessor* current = nullptr;

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

        PluginHolder* active = nullptr;
        float lastSentParam[numParams] { -1.0f, -1.0f };
        juce::MidiBuffer midi;

        // The dry signal held back by the plugin's latency, so the two meet in
        // step instead of combing.
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 1 };
        int dryDelaySamples = 0;
    };

    void processSlot (Slot& slot, juce::AudioBuffer<float>& buffer, int start, int numSamples);
    bool swapPluginIfPossible (Slot& slot);
    void preparePlugin (juce::AudioProcessor& processor) const;

    std::array<Slot, numSlots> slots;
    std::atomic<double> beatSeconds { 0.5 };
    double sampleRate = 44100.0;

    // What a plugin is prepared with, and a lock kept by setPlugin and
    // prepare alone, so the two never prepare one plugin at once. The audio
    // callback never takes it.
    int preparedBlockSize = 512;
    juce::CriticalSection prepareLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterEffects)
};

} // namespace opendj
