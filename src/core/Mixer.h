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

/** The mixer section: one strip per deck, a crossfader, master and cue busses.

    The three band EQ is a Linkwitz-Riley crossover rather than a stack of shelf
    filters, so a band at zero is a true kill and the bands sum back flat when
    they are all at unity. Every gain is smoothed, so knobs and faders never
    click even when a controller sends coarse steps.
*/
class Mixer
{
public:
    static constexpr int numChannels = 4;

    /** Where the three band EQ splits the signal.

        Public because the waveform is coloured by the same three bands, and a
        waveform whose red does not mean what the Low knob reaches would be
        worse than no colour at all. */
    static constexpr float lowCrossoverHz = 300.0f;
    static constexpr float highCrossoverHz = 3000.0f;

    /** Which side of the crossfader a channel answers to.

        Two channels and a crossfader needs no such switch, because A and B are
        the only answers. Four does: the two extra decks are usually wanted at
        full level regardless of where the crossfader sits, which is what
        `thru` means and why it is the default for channels C and D. */
    enum class CrossfaderAssign
    {
        a,
        thru,   ///< ignores the crossfader entirely
        b
    };

    enum class CrossfaderCurve
    {
        constantPower,   ///< gentle blend, for long mixes
        linear,
        sharpCut         ///< near instant, for scratching and cutting
    };

    Mixer();

    //==========================================================================
    // Message thread
    //==========================================================================

    void setChannelFader (int channel, float normalised);          // 0 to 1
    void setChannelEq (int channel, int band, float normalised);   // 0 to 1, 0.5 is flat
    void setChannelCue (int channel, bool shouldMonitor);

    /** Which side of the crossfader a channel follows, or neither. */
    void setChannelCrossfaderAssign (int channel, CrossfaderAssign assign);
    CrossfaderAssign getChannelCrossfaderAssign (int channel) const noexcept;

    /** One knob per channel, centred at 0.5 and doing nothing there. Turned
        down it is a low pass sweeping out of the top; turned up, a high pass
        sweeping out of the bottom. Both filters run at all times, transparent
        at their ends, so the knob never switches type mid-signal and clicks. */
    void setChannelFilter (int channel, float normalised);

    /** A beat-synced echo per channel.

        One knob: at zero it is silent and out of the way, and turning it up
        raises both how much comes back and how long it takes to die away, which
        is how a DJ echo is actually used. The repeats are always audible, never
        a wash, because an echo you cannot hear the taps of is just reverb. */
    void setChannelEcho (int channel, float amount);
    float getChannelEcho (int channel) const noexcept;

    /** How long one repeat lasts, in seconds. Set from the deck's beat length
        by the engine, since the mixer has no idea what tempo anything is. */
    void setChannelEchoTime (int channel, double seconds);
    double getChannelEchoTime (int channel) const noexcept;

    /** A reverb per channel, on one knob. Silent at zero, and the tail that is
        already ringing when it is turned down is allowed to finish rather than
        being cut off. */
    void setChannelReverb (int channel, float amount);
    float getChannelReverb (int channel) const noexcept;
    void toggleChannelCue (int channel);
    bool isChannelCued (int channel) const;

    void setCrossfaderPosition (float position);                   // -1 (A) to +1 (B)
    void setCrossfaderCurve (CrossfaderCurve newCurve);

    void setMasterGain (float normalised);                         // 0 to 1
    void setCueGain (float normalised);                            // 0 to 1
    void setCueMix (float normalised);                             // 0 cue only, 1 master only

    float getMasterPeak (int channel) const noexcept;

    //==========================================================================
    // Where every control is, so that the interface can follow a controller.
    //
    // The positions are kept as they were set rather than worked back out of
    // the gains: the fader and EQ curves are not worth inverting, and a knob
    // that came back a fraction different from where it was put would creep.
    //==========================================================================

    float getChannelFader (int channel) const noexcept;
    float getChannelEq (int channel, int band) const noexcept;
    float getChannelFilter (int channel) const noexcept;
    float getCrossfaderPosition() const noexcept { return crossfaderPosition.load (std::memory_order_relaxed); }
    float getMasterGain() const noexcept { return masterPosition.load (std::memory_order_relaxed); }
    float getCueGain() const noexcept    { return cuePosition.load (std::memory_order_relaxed); }
    float getCueMix() const noexcept     { return targetCueMix.load (std::memory_order_relaxed); }

    CrossfaderCurve getCrossfaderCurve() const noexcept { return curve.load (std::memory_order_relaxed); }

    //==========================================================================
    // Audio thread
    //==========================================================================

    void prepare (double sampleRate, int blockSize);
    void reset();

    /** Mixes the per-deck buffers into the master and cue busses. All buffers
        are stereo and the same length; master and cue are overwritten. */
    void processBlock (const std::array<juce::AudioBuffer<float>*, numChannels>& deckBuffers,
                       juce::AudioBuffer<float>& master,
                       juce::AudioBuffer<float>& cue);

private:
    struct ChannelStrip
    {
        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();

        juce::dsp::LinkwitzRileyFilter<float> lowSplit;    // crossover at lowCrossoverHz
        juce::dsp::LinkwitzRileyFilter<float> highSplit;   // crossover at highCrossoverHz
        juce::dsp::LinkwitzRileyFilter<float> lowAllpass;  // phase match for the low band

        juce::dsp::StateVariableTPTFilter<float> filterLow;    // the knob turned down
        juce::dsp::StateVariableTPTFilter<float> filterHigh;   // the knob turned up
        juce::SmoothedValue<float> filterLowCutoff, filterHighCutoff;

        // The echo. Its length is smoothed rather than stepped, so changing the
        // beat division sweeps the repeats the way a tape delay does instead of
        // clicking, which is an effect in its own right and the one DJs expect.
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> echo { 1 };
        juce::SmoothedValue<float> echoDelaySamples;
        juce::SmoothedValue<float> echoWet, echoFeedback;

        std::atomic<float> echoAmount { 0.0f };
        std::atomic<double> echoSeconds { 0.5 };

        // Reverb runs on a block rather than a sample at a time, which is why
        // the channel is built into a buffer first and mixed down afterwards.
        // It is fed at full wet into its own buffer and mixed in per sample
        // with a smoothed gain, so the knob cannot click and a tail already
        // ringing is never cut off.
        juce::Reverb reverb;
        juce::AudioBuffer<float> shapedBuffer, reverbBuffer;
        juce::SmoothedValue<float> reverbWet;
        std::atomic<float> reverbAmount { 0.0f };

        std::array<juce::SmoothedValue<float>, 3> bandGain;
        juce::SmoothedValue<float> faderGain;
        juce::SmoothedValue<float> crossfaderGain;

        std::atomic<float> targetBandGain[3] { { 1.0f }, { 1.0f }, { 1.0f } };
        std::atomic<float> targetFaderGain { 0.0f };

        // The normalised positions behind those gains.
        std::atomic<float> bandPosition[3] { { 0.5f }, { 0.5f }, { 0.5f } };
        std::atomic<float> faderPosition { 0.0f };
        std::atomic<float> filterPosition { 0.5f };
        std::atomic<float> targetCrossfaderGain { 1.0f };
        std::atomic<CrossfaderAssign> crossfaderAssign { CrossfaderAssign::thru };
        std::atomic<bool> cueEnabled { false };
    };

    void recalculateCrossfader();

    std::array<ChannelStrip, numChannels> strips;

    std::atomic<float> crossfaderPosition { 0.0f };
    std::atomic<CrossfaderCurve> curve { CrossfaderCurve::constantPower };

    juce::SmoothedValue<float> masterGain;
    juce::SmoothedValue<float> cueGain;
    juce::SmoothedValue<float> cueMix;

    std::atomic<float> masterPosition { 1.0f };
    std::atomic<float> cuePosition { 0.7f };
    std::atomic<float> targetMasterGain { 1.0f };
    std::atomic<float> targetCueGain { 0.7f };
    std::atomic<float> targetCueMix { 0.0f };

    std::atomic<float> masterPeak[2] { { 0.0f }, { 0.0f } };

    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mixer)
};

} // namespace opendj
