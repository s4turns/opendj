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
        plugin,
        flanger,
        phaser,
        gate,
        bitcrusher,
        stutter
    };

    static constexpr int numTypes = 9;

    /** The order effects are offered in, and stepped through. The numbers above
        stay as they are because a mapping file can name one (master_fx.type),
        and settings files name them in words, so neither cares about this. */
    static constexpr std::array<Type, numTypes> menuOrder
    {
        Type::echo, Type::reverb, Type::filter, Type::flanger, Type::phaser,
        Type::gate, Type::bitcrusher, Type::stutter, Type::plugin
    };

    static juce::String getTypeName (Type type);

    /** What a parameter knob does for an effect, short enough for a label.
        A plugin's knobs are named by getParamLabel, which knows the plugin. */
    static juce::String getParamName (Type type, int param);

    /** The echo lengths, in beats, that the first echo parameter snaps to. */
    static constexpr std::array<double, 6> echoDivisions { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    static int echoDivisionIndexFor (float normalised) noexcept;
    static float normalisedForEchoDivision (int index) noexcept;
    static juce::String echoDivisionName (int index);

    /** How long one sweep of the flanger or phaser lasts, in beats. */
    static constexpr std::array<double, 6> sweepDivisions { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };

    /** The musical length a parameter stands for at this position, for a label:
        "1/4" for the echo, the gate and the stutter, "8" for a sweep. Empty for
        a parameter that is not a length. */
    static juce::String getDivisionText (Type type, int param, float normalised);

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

    /** Where the mix is, in beats since the track's first. The gate and the
        sweeps run on a clock of their own between calls, so this only has to
        arrive now and then; the engine's timer sends it. A small difference is
        eased out and a large one, from a seek, is jumped to. */
    void setBeatPosition (double beats);

    /** The clock itself, in beats, wrapped at beatCycle. */
    double getBeatClock() const noexcept { return beatClockForUi.load (std::memory_order_relaxed); }

    /** Every division above fits into this many beats a whole number of times,
        so wrapping the clock never moves a gate or a sweep. */
    static constexpr double beatCycle = 64.0;

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

    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept     { return preparedBlockSize; }

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
        std::atomic<float> wet[numTypes] { { 0.5f }, { 0.4f }, { 1.0f }, { 1.0f },
                                           { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f } };
        std::atomic<float> params[numTypes][numParams]
        {
            { { 0.6f }, { 0.5f } },     // one beat, moderate feedback
            { { 0.6f }, { 0.4f } },     // a medium room
            { { 0.5f }, { 0.3f } },     // open, a little resonance
            { { 0.5f }, { 0.5f } },     // replaced by the plugin's own values
            { { 0.4f }, { 0.4f } },     // flanger: four beats, some feedback
            { { 0.4f }, { 0.5f } },     // phaser: four beats, moderate depth
            { { 0.4f }, { 0.5f } },     // gate: half beats, open half the time
            { { 0.5f }, { 0.3f } },     // bitcrusher: nine bits, a little aliasing
            { { 0.2f }, { 0.0f } }      // stutter: quarter beats that do not fade
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

        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> flangerLine { 1 };
        float flangerDelayNow[2] { 0.0f, 0.0f };

        static constexpr int phaserStages = 6;
        float phaserIn[2][phaserStages] {}, phaserOut[2][phaserStages] {};
        float phaserLast[2] { 0.0f, 0.0f }, phaserCoefficient[2] { 0.0f, 0.0f };
        int phaserCountdown = 0;

        juce::SmoothedValue<float> gateGain;

        float crushHeld[2] { 0.0f, 0.0f };
        double crushCounter = 0.0;

        // The last few seconds of the mix, which a stutter repeats the end of.
        // It stops being written while a slice is held, so a long hold cannot
        // overwrite what it is playing.
        juce::AudioBuffer<float> stutterRing;
        int stutterWrite = 0, stutterStart = 0, stutterLength = 0, stutterOffset = 0;
        float stutterGain = 1.0f;
        bool stutterHeld = false;

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
    std::atomic<double> beatPositionHint { -1.0 };
    std::atomic<double> beatClockForUi { 0.0 };
    double beatClock = 0.0;                                // audio thread
    double sampleRate = 44100.0;

    // What a plugin is prepared with, and a lock kept by setPlugin and
    // prepare alone, so the two never prepare one plugin at once. The audio
    // callback never takes it.
    int preparedBlockSize = 512;
    juce::CriticalSection prepareLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterEffects)
};

} // namespace opendj
