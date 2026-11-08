/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/Loudness.h"

#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace opendj::loudness;

namespace
{
    float fromDb (double db) { return (float) std::pow (10.0, db / 20.0); }

    /** A stereo sine, with both channels the same. */
    juce::AudioBuffer<float> sine (double frequency, double seconds, float amplitude, double sampleRate)
    {
        juce::AudioBuffer<float> audio (2, (int) (seconds * sampleRate));
        const auto delta = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const auto value = (float) std::sin (delta * i) * amplitude;
            audio.setSample (0, i, value);
            audio.setSample (1, i, value);
        }

        return audio;
    }

    void append (juce::AudioBuffer<float>& audio, const juce::AudioBuffer<float>& more)
    {
        const auto was = audio.getNumSamples();
        audio.setSize (2, was + more.getNumSamples(), true);

        for (int ch = 0; ch < 2; ++ch)
            audio.copyFrom (ch, was, more, ch, 0, more.getNumSamples());
    }
}

TEST_CASE ("the reference tone reads as the standard says it must", "[loudness]")
{
    // EBU Tech 3341's own check: a stereo 997 Hz sine at -23 dBFS reads -23 LUFS,
    // to within a tenth.
    REQUIRE_THAT (measureLufs (sine (997.0, 20.0, fromDb (-23.0), 48000.0), 48000.0), WithinAbs (-23.0, 0.1));
    REQUIRE_THAT (measureLufs (sine (997.0, 20.0, fromDb (-33.0), 48000.0), 48000.0), WithinAbs (-33.0, 0.1));
    REQUIRE_THAT (measureLufs (sine (997.0, 20.0, fromDb (-6.0), 48000.0), 48000.0), WithinAbs (-6.0, 0.1));
}

TEST_CASE ("the sample rate does not change the answer", "[loudness]")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        INFO (rate);
        REQUIRE_THAT (measureLufs (sine (997.0, 10.0, fromDb (-23.0), rate), rate), WithinAbs (-23.0, 0.1));
    }
}

TEST_CASE ("the weighting hears as an ear does, not as a meter does", "[loudness]")
{
    constexpr double rate = 48000.0;
    const auto amplitude = fromDb (-20.0);

    // At the same level on a meter, the deep end counts for far less and the top
    // end for a little more.
    const auto middle = measureLufs (sine (1000.0, 5.0, amplitude, rate), rate);
    const auto deep = measureLufs (sine (25.0, 5.0, amplitude, rate), rate);
    const auto top = measureLufs (sine (8000.0, 5.0, amplitude, rate), rate);

    REQUIRE (deep < middle - 3.0);
    REQUIRE (top > middle + 2.0);
}

TEST_CASE ("silence does not make a track quieter", "[loudness]")
{
    constexpr double rate = 48000.0;

    auto audio = sine (997.0, 20.0, fromDb (-23.0), rate);
    juce::AudioBuffer<float> gap (2, (int) (40.0 * rate));
    gap.clear();
    append (audio, gap);

    REQUIRE_THAT (measureLufs (audio, rate), WithinAbs (-23.0, 0.1));
}

TEST_CASE ("a long quiet part does not make a track quieter", "[loudness]")
{
    constexpr double rate = 48000.0;

    // Thirty dB down is under the relative gate, so only the loud part counts.
    auto audio = sine (997.0, 20.0, fromDb (-23.0), rate);
    append (audio, sine (997.0, 60.0, fromDb (-53.0), rate));

    REQUIRE_THAT (measureLufs (audio, rate), WithinAbs (-23.0, 0.2));
}

TEST_CASE ("a part only a little quieter does count", "[loudness]")
{
    constexpr double rate = 48000.0;

    // Six dB down is over the gate, so the answer falls between the two.
    auto audio = sine (997.0, 20.0, fromDb (-23.0), rate);
    append (audio, sine (997.0, 20.0, fromDb (-29.0), rate));

    const auto lufs = measureLufs (audio, rate);
    REQUIRE (lufs < -23.5);
    REQUIRE (lufs > -29.0);
}

TEST_CASE ("nothing to measure reads as not known", "[loudness]")
{
    juce::AudioBuffer<float> silent (2, 48000 * 5);
    silent.clear();

    REQUIRE (measureLufs (silent, 48000.0) == 0.0);
    REQUIRE (measureLufs (juce::AudioBuffer<float>(), 48000.0) == 0.0);
    REQUIRE (measureLufs (sine (997.0, 0.2, 0.5f, 48000.0), 48000.0) == 0.0);    // too short for one block
    REQUIRE (measureLufs (sine (997.0, 5.0, 0.5f, 48000.0), 0.0) == 0.0);
}

TEST_CASE ("a mono file reads the same as the same thing in stereo", "[loudness]")
{
    constexpr double rate = 48000.0;
    const auto stereo = sine (997.0, 10.0, fromDb (-23.0), rate);

    juce::AudioBuffer<float> mono (1, stereo.getNumSamples());
    mono.copyFrom (0, 0, stereo, 0, 0, stereo.getNumSamples());

    REQUIRE_THAT (measureLufs (mono, rate), WithinAbs (-23.0, 0.1));
}

TEST_CASE ("the gain takes a track to the target, within its limits", "[loudness]")
{
    // A hot master comes down, and an old one comes up.
    REQUIRE_THAT (gainForLoudness (-8.0, 1.0f), WithinAbs (fromDb (-6.0), 1.0e-4));
    REQUIRE_THAT (gainForLoudness (-20.0, 0.25f), WithinAbs (fromDb (6.0), 1.0e-4));
    REQUIRE_THAT (gainForLoudness (targetLufs, 1.0f), WithinAbs (1.0, 1.0e-6));

    REQUIRE_THAT (gainForLoudness (-60.0, 0.001f), WithinAbs (loudestGain, 1.0e-4));
    REQUIRE_THAT (gainForLoudness (-0.1, 1.0f), WithinAbs (fromDb (-13.9) > quietestGain ? fromDb (-13.9) : quietestGain, 1.0e-4));
}

TEST_CASE ("the gain never lifts a track past its own peak", "[loudness]")
{
    // It would like +6 dB and has only 3 dB of headroom.
    REQUIRE_THAT (gainForLoudness (-20.0, 0.7f), WithinAbs (1.0 / 0.7, 1.0e-4));

    // No headroom means no lift, rather than a cut.
    REQUIRE_THAT (gainForLoudness (-20.0, 1.0f), WithinAbs (1.0, 1.0e-6));
    REQUIRE_THAT (gainForLoudness (-20.0, 1.3f), WithinAbs (1.0, 1.0e-6));

    // A cut does not care.
    REQUIRE_THAT (gainForLoudness (-8.0, 1.3f), WithinAbs (fromDb (-6.0), 1.0e-4));
}

TEST_CASE ("a loudness that is not known changes nothing", "[loudness]")
{
    REQUIRE (gainForLoudness (0.0, 0.5f) == 1.0f);
    REQUIRE (gainForLoudness (std::nan (""), 0.5f) == 1.0f);
    REQUIRE (gainForLoudness (-std::numeric_limits<double>::infinity(), 0.5f) == 1.0f);
}
