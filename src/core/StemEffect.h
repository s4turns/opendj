/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>

namespace opendj
{

/** One effect on one stem of a deck: an echo on the vocals, say, with the drums
    left dry.

    It runs ahead of the time stretcher, at the place the deck still has the
    stems apart, because under key lock that is the only place it does. So the
    repeats stay in key and on the beat as the fader moves. What comes out is not
    a replacement for the stem but a difference to add to whatever the deck was
    going to play anyway: the wet signal for an echo or a reverb, and the filtered
    stem minus the stem for the low pass. A deck playing its mix keeps playing its
    mix, with no jump to the slightly different sound of the stems summed.

    Echo and reverb are sends, so a stem pad or a knob closing the stem closes the
    way in and leaves the tail ringing. The low pass has nothing to ring.

    Everything is built in prepare(). After that it belongs to the audio thread,
    and is told what to do once a block.
*/
class StemEffect
{
public:
    StemEffect();
    ~StemEffect();

    enum class Type
    {
        echo = 0,
        reverb,
        lowPass
    };

    static constexpr int numTypes = 3;
    static juce::String getTypeName (Type type);

    /** The echo lengths on offer, in beats. */
    static constexpr std::array<double, 4> beatDivisions { 0.25, 0.5, 1.0, 2.0 };
    static constexpr int numDivisions = (int) beatDivisions.size();
    static juce::String getDivisionName (int division);

    /** The longest repeat, and so the length of the delay line. A deck can be
        slowed to a twentieth of its speed, which at two beats would otherwise be
        a repeat a minute long, and no line is long enough for that. */
    static constexpr double longestDelaySeconds = 4.0;

    /** How long one repeat lasts, ready for the line. `beatSeconds` is a beat of
        the track at its recorded speed. A stretcher scales what it is fed, so a
        stretched repeat is measured at the recorded speed and comes out right at
        any tempo. Off the stretcher, the tempo has to be allowed for here. */
    static double delaySecondsFor (double beatSeconds, int division, double tempoRatio, bool stretching) noexcept;

    /** What the deck wants this block. */
    struct Settings
    {
        bool on = false;
        int stem = 0;
        Type type = Type::echo;
        float amount = 0.0f;
        double delaySeconds = 0.5;
    };

    void prepare (double sampleRate);

    /** Forgets everything in it, at once. Audio thread. */
    void reset();

    /** True from the first block run until reset, or until the tail has died. */
    bool isRinging() const noexcept { return ringing; }

    /** Call once a block, before any tick. Returns false once there is nothing
        left to do: it is off and silent, and the deck can stop calling it. */
    bool beginBlock (const Settings& settings) noexcept;

    /** The stem to read for the next tick. A change of target waits for the way
        in to fade down first, so this is not always the one asked for. */
    int getStem() const noexcept { return activeStem; }

    /** One frame of the stem in, and the difference to add to the dry signal. */
    void tick (float inLeft, float inRight, float& deltaLeft, float& deltaRight) noexcept;

private:
    void clearStateOf (Type type);

    // The processors are kept in the .cpp, so that the deck, and everything that
    // includes the deck, does not take on the whole of juce_dsp along with them.
    struct Processors;
    std::unique_ptr<Processors> processors;

    juce::SmoothedValue<float> delaySamples;
    juce::SmoothedValue<float> amountRamp;
    juce::SmoothedValue<float> cutoffRamp;

    double sampleRate = 44100.0;
    float fadeStep = 1.0f;
    juce::int64 tailHoldSamples = 0;

    Settings wanted;
    Type activeType = Type::echo;
    int activeStem = 0;
    float send = 0.0f;              // how far open the way in is
    float output = 1.0f;            // comes down only for a change of effect
    juce::int64 silentFor = 0;
    bool ringing = false;
};

} // namespace opendj
