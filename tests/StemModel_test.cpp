/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>

#include "analysis/StemModel.h"

#include <cmath>

namespace
{
    double rms (const juce::AudioBuffer<float>& buffer)
    {
        double sum = 0.0;
        long count = 0;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i, ++count)
                sum += (double) buffer.getSample (ch, i) * buffer.getSample (ch, i);

        return count > 0 ? std::sqrt (sum / (double) count) : 0.0;
    }
}

/** Needs the htdemucs weights and an OpenVINO build, so it skips with a
    warning where either is missing rather than failing a machine that never
    asked for stems. The synthetic mix cannot say how good a separation is;
    it says the pipeline is wired up: the model loads, a track of an awkward
    length goes in and comes out the same length as four well formed stems, and
    the parts add back up to roughly what went in. */
TEST_CASE ("the model separates a short mix into four stems that add back up", "[stems][model]")
{
    opendj::StemModel model;

    if (opendj::StemModel::findModel() == juce::File())
    {
        WARN ("Skipping: no htdemucs model in " << opendj::StemModel::getSearchedLocations().joinIntoString (", "));
        return;
    }

    if (const auto error = model.load(); error.isNotEmpty())
    {
        WARN ("Skipping: " << error);
        return;
    }

    REQUIRE (model.isLoaded());

    // Ten seconds and a bit: more than one model segment, and not a whole
    // number of anything, so the blending of segments is exercised too.
    constexpr int numSamples = 441000 + 1234;
    juce::AudioBuffer<float> mix (2, numSamples);
    juce::Random random (7);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = i / 44100.0;
        const auto bass = 0.3 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);
        const auto lead = 0.2 * std::sin (juce::MathConstants<double>::twoPi * (440.0 + 40.0 * std::sin (t)) * t);
        const auto hat = std::fmod (t, 0.5) < 0.03 ? 0.2 * (random.nextDouble() * 2.0 - 1.0) : 0.0;

        mix.setSample (0, i, (float) (bass + lead + hat));
        mix.setSample (1, i, (float) (bass + 0.8 * lead + hat));
    }

    opendj::SeparatedTrack stems;
    REQUIRE (model.separate (mix, stems));

    REQUIRE (stems.isWellFormed());
    REQUIRE (stems.getNumSamples() == numSamples);

    juce::AudioBuffer<float> sum (2, numSamples);
    sum.clear();

    for (const auto& stem : stems.stems)
        for (int ch = 0; ch < 2; ++ch)
            sum.addFrom (ch, 0, stem, ch, 0, numSamples);

    const auto input = rms (mix);
    const auto output = rms (sum);

    INFO ("input rms " << input << ", summed stems rms " << output);
    REQUIRE (input > 0.05);
    REQUIRE (output > input * 0.5);
    REQUIRE (output < input * 1.5);

    // Not four copies of the same thing, and not silence.
    auto distinct = false;

    for (size_t i = 1; i < stems.stems.size(); ++i)
        distinct = distinct || std::abs (rms (stems.stems[i]) - rms (stems.stems[0])) > 1.0e-4;

    REQUIRE (distinct);
}
