/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/Loudness.h"

#include <cmath>
#include <vector>

namespace opendj::loudness
{

namespace
{
    constexpr double absoluteGateLufs = -70.0;
    constexpr double relativeGateLu = -10.0;
    constexpr double stepSeconds = 0.1;         // a block is four steps: 400 ms, overlapping by 75%
    constexpr int stepsPerBlock = 4;
    constexpr double calibration = -0.691;      // what BS.1770 adds, so a 997 Hz tone reads true

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double process (double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /** The head's high shelf, from the standard's own figures. The coefficients
        it prints are for 48 kHz only, so they are derived for the rate at hand. */
    Biquad preFilter (double sampleRate) noexcept
    {
        constexpr double frequency = 1681.974450955533;
        constexpr double gainDb = 3.999843853973347;
        constexpr double q = 0.7071752369554196;

        const auto k = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);
        const auto vh = std::pow (10.0, gainDb / 20.0);
        const auto vb = std::pow (vh, 0.4996667741545416);
        const auto a0 = 1.0 + k / q + k * k;

        Biquad filter;
        filter.b0 = (vh + vb * k / q + k * k) / a0;
        filter.b1 = 2.0 * (k * k - vh) / a0;
        filter.b2 = (vh - vb * k / q + k * k) / a0;
        filter.a1 = 2.0 * (k * k - 1.0) / a0;
        filter.a2 = (1.0 - k / q + k * k) / a0;
        return filter;
    }

    /** And its high pass, which an ear also has and a sub-bass meter does not. */
    Biquad highPass (double sampleRate) noexcept
    {
        constexpr double frequency = 38.13547087602444;
        constexpr double q = 0.5003270373238773;

        const auto k = std::tan (juce::MathConstants<double>::pi * frequency / sampleRate);
        const auto a0 = 1.0 + k / q + k * k;

        Biquad filter;
        filter.b0 = 1.0;
        filter.b1 = -2.0;
        filter.b2 = 1.0;
        filter.a1 = 2.0 * (k * k - 1.0) / a0;
        filter.a2 = (1.0 - k / q + k * k) / a0;
        return filter;
    }

    double toLufs (double energy) noexcept
    {
        return calibration + 10.0 * std::log10 (energy);
    }

    double fromLufs (double lufs) noexcept
    {
        return std::pow (10.0, (lufs - calibration) / 10.0);
    }
}

double measureLufs (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto numSamples = audio.getNumSamples();
    const auto numChannels = juce::jmin (2, audio.getNumChannels());
    const auto stepLength = (int) std::round (stepSeconds * sampleRate);

    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0 || numSamples < stepLength * stepsPerBlock)
        return 0.0;

    // The weighted energy of each step, summed over the channels. A mono file
    // counts as though it played from both speakers, which it does here.
    std::vector<double> steps ((size_t) (numSamples / stepLength), 0.0);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto shelf = preFilter (sampleRate);
        auto pass = highPass (sampleRate);
        const auto* data = audio.getReadPointer (ch);
        const auto weight = numChannels == 1 ? 2.0 : 1.0;

        for (size_t step = 0; step < steps.size(); ++step)
        {
            auto sum = 0.0;

            for (int i = 0; i < stepLength; ++i)
            {
                const auto y = pass.process (shelf.process (data[(int) step * stepLength + i]));
                sum += y * y;
            }

            steps[step] += sum * weight;
        }
    }

    // Overlapping blocks, as energies. The sums are kept linear throughout, and
    // only the gates are in LUFS.
    std::vector<double> blocks;
    blocks.reserve (steps.size());

    const auto blockLength = (double) stepLength * stepsPerBlock;

    for (size_t start = 0; start + stepsPerBlock <= steps.size(); ++start)
    {
        auto sum = 0.0;

        for (int s = 0; s < stepsPerBlock; ++s)
            sum += steps[start + (size_t) s];

        blocks.push_back (sum / blockLength);
    }

    const auto gatedMean = [&blocks] (double gateEnergy)
    {
        auto sum = 0.0;
        auto count = 0;

        for (const auto block : blocks)
            if (block > gateEnergy)
            {
                sum += block;
                ++count;
            }

        return count > 0 ? sum / count : 0.0;
    };

    const auto ungated = gatedMean (fromLufs (absoluteGateLufs));

    if (ungated <= 0.0)
        return 0.0;

    const auto relativeGate = juce::jmax (fromLufs (absoluteGateLufs), ungated * std::pow (10.0, relativeGateLu / 10.0));
    const auto result = gatedMean (relativeGate);

    // Zero is taken to mean not measured, and a square wave at full scale really
    // can read above it.
    return result > 0.0 ? juce::jmin (-0.01, toLufs (result)) : 0.0;
}

float gainForLoudness (double lufs, float peak) noexcept
{
    if (lufs >= 0.0 || ! std::isfinite (lufs))
        return 1.0f;

    auto gain = juce::jlimit (quietestGain, loudestGain, (float) std::pow (10.0, (targetLufs - lufs) / 20.0));

    if (gain > 1.0f && peak > 0.0f)
        gain = juce::jmin (gain, juce::jmax (1.0f, 1.0f / peak));

    return gain;
}

} // namespace opendj::loudness
