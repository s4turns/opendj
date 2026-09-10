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
    static constexpr int numChannels = 2;

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
    void toggleChannelCue (int channel);
    bool isChannelCued (int channel) const;

    void setCrossfaderPosition (float position);                   // -1 (A) to +1 (B)
    void setCrossfaderCurve (CrossfaderCurve newCurve);

    void setMasterGain (float normalised);                         // 0 to 1
    void setCueGain (float normalised);                            // 0 to 1
    void setCueMix (float normalised);                             // 0 cue only, 1 master only

    float getMasterPeak (int channel) const noexcept;

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

        std::array<juce::SmoothedValue<float>, 3> bandGain;
        juce::SmoothedValue<float> faderGain;
        juce::SmoothedValue<float> crossfaderGain;

        std::atomic<float> targetBandGain[3] { { 1.0f }, { 1.0f }, { 1.0f } };
        std::atomic<float> targetFaderGain { 0.0f };
        std::atomic<float> targetCrossfaderGain { 1.0f };
        std::atomic<bool> cueEnabled { false };
    };

    void recalculateCrossfader();

    static constexpr float lowCrossoverHz = 300.0f;
    static constexpr float highCrossoverHz = 3000.0f;

    std::array<ChannelStrip, numChannels> strips;

    std::atomic<float> crossfaderPosition { 0.0f };
    std::atomic<CrossfaderCurve> curve { CrossfaderCurve::constantPower };

    juce::SmoothedValue<float> masterGain;
    juce::SmoothedValue<float> cueGain;
    juce::SmoothedValue<float> cueMix;

    std::atomic<float> targetMasterGain { 1.0f };
    std::atomic<float> targetCueGain { 0.7f };
    std::atomic<float> targetCueMix { 0.0f };

    std::atomic<float> masterPeak[2] { { 0.0f }, { 0.0f } };

    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mixer)
};

} // namespace opendj
