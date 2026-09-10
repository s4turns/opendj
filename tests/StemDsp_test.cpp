/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/StemDsp.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
namespace dsp = opendj::stemdsp;

namespace
{
    /** The signal the reference values below were produced from. Three tones,
        so that it can be written identically here and in numpy. */
    std::vector<float> referenceSignal (int n = 8192)
    {
        std::vector<float> x ((size_t) n);
        constexpr double rate = 44100.0;
        constexpr auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (0.5 * std::sin (twoPi * 440.0 * i / rate)
                                   + 0.25 * std::sin (twoPi * 1000.0 * i / rate)
                                   + 0.1 * std::cos (twoPi * 57.0 * i / rate));

        return x;
    }

    struct BinReference { int bin, frame; float real, imaginary; };
}

TEST_CASE ("the window is torch's periodic Hann, not the symmetric one", "[stems][dsp]")
{
    const auto w = dsp::hannPeriodic (4096);

    REQUIRE (w.size() == 4096);
    REQUIRE_THAT (w[0],    WithinAbs (0.0f, 1.0e-7f));
    REQUIRE_THAT (w[1024], WithinAbs (0.5f, 1.0e-6f));
    REQUIRE_THAT (w[2048], WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (w[3072], WithinAbs (0.5f, 1.0e-6f));

    // The periodic window never returns to zero at the end; the symmetric one
    // does, and using it would put a different signal into the model.
    REQUIRE (w[4095] > 0.0f);
    REQUIRE_THAT (w[4095], WithinAbs (5.88e-7f, 1.0e-8f));
}

TEST_CASE ("the STFT matches torch bin for bin", "[stems][dsp]")
{
    // Produced by stemsep's numpy, which is itself checked against torch.stft
    // with the same flags to about 1e-5.
    static constexpr BinReference expected[]
    {
        { 0, 0, 0.30959198f, 0.00000000f },
        { 1, 2, -0.00429074f, -0.00299903f },
        { 40, 3, -3.88848972f, 2.81247735f },
        { 41, 3, 6.40868998f, -4.63529062f },
        { 93, 4, 2.71606278f, 2.88571382f },
        { 500, 7, -0.00015375f, -0.00014935f },
    };

    const auto x = referenceSignal();
    const auto z = dsp::stft (x.data(), (int) x.size());

    REQUIRE (z.bins == 2049);
    REQUIRE (z.frames == 9);

    for (const auto& e : expected)
    {
        INFO ("bin " << e.bin << " frame " << e.frame);
        REQUIRE_THAT (z.re (e.bin, e.frame), WithinAbs (e.real, 1.0e-4f));
        REQUIRE_THAT (z.im (e.bin, e.frame), WithinAbs (e.imaginary, 1.0e-4f));
    }
}

TEST_CASE ("the inverse STFT gives the signal back", "[stems][dsp]")
{
    const auto x = referenceSignal();
    const auto z = dsp::stft (x.data(), (int) x.size());
    const auto y = dsp::istft (z, (int) x.size());

    REQUIRE (y.size() == x.size());

    auto worst = 0.0f;

    for (size_t i = 0; i < x.size(); ++i)
        worst = juce::jmax (worst, std::abs (y[i] - x[i]));

    INFO ("largest difference: " << worst);
    REQUIRE (worst < 1.0e-5f);
}

TEST_CASE ("spec drops the Nyquist bin and the padding frames", "[stems][dsp]")
{
    static constexpr BinReference expected[]
    {
        { 0, 0, 0.25641215f, 0.00000000f },
        { 1, 1, 0.03862462f, -0.03172000f },
        { 40, 2, -1.25032377f, 4.63324642f },
        { 93, 3, -3.93213010f, -0.49275863f },
    };

    const auto x = referenceSignal();
    const auto z = dsp::spec (x.data(), (int) x.size());

    // 2048 bins, not 2049: the model is never shown the Nyquist bin. Eight
    // frames, not twelve: two at each end are padding the model never sees.
    REQUIRE (z.bins == dsp::freqBins);
    REQUIRE (z.frames == 8);

    for (const auto& e : expected)
    {
        INFO ("bin " << e.bin << " frame " << e.frame);
        REQUIRE_THAT (z.re (e.bin, e.frame), WithinAbs (e.real, 1.0e-4f));
        REQUIRE_THAT (z.im (e.bin, e.frame), WithinAbs (e.imaginary, 1.0e-4f));
    }
}

TEST_CASE ("ispec reconstructs the interior of the signal", "[stems][dsp]")
{
    // Only the interior: spec throws the Nyquist bin away, so the very edges
    // cannot come back exactly. The model never uses them either, because
    // segments are overlapped and blended.
    const auto x = referenceSignal();
    const auto z = dsp::spec (x.data(), (int) x.size());
    const auto y = dsp::ispec (z, (int) x.size());

    REQUIRE (y.size() == x.size());

    auto worst = 0.0f;

    for (size_t i = 2048; i < 6144; ++i)
        worst = juce::jmax (worst, std::abs (y[i] - x[i]));

    INFO ("largest interior difference: " << worst);
    REQUIRE (worst < 1.0e-4f);

    // And against the values numpy produced at the same places.
    REQUIRE_THAT (y[2048], WithinAbs (0.23462681f, 1.0e-4f));
    REQUIRE_THAT (y[4096], WithinAbs (-0.56934756f, 1.0e-4f));
    REQUIRE_THAT (y[6143], WithinAbs (0.81585389f, 1.0e-4f));
}

TEST_CASE ("a segment-length signal gives the shape the model demands", "[stems][dsp]")
{
    // The converted graph has fixed input shapes, so this is not a preference.
    std::vector<float> x ((size_t) dsp::segment, 0.0f);

    for (int i = 0; i < dsp::segment; ++i)
        x[(size_t) i] = (float) std::sin (i * 0.01);

    const auto z = dsp::spec (x.data(), dsp::segment);

    REQUIRE (z.bins == 2048);
    REQUIRE (z.frames == 336);
    REQUIRE (dsp::frames == 336);
}

TEST_CASE ("reflect padding follows numpy rather than repeating the edge", "[stems][dsp]")
{
    const float x[] { 1.0f, 2.0f, 3.0f, 4.0f };
    const auto padded = dsp::pad1d (x, 4, 2, 2);

    REQUIRE (padded.size() == 8);

    // 3 2 | 1 2 3 4 | 3 2 -- the edge sample itself is not repeated.
    REQUIRE_THAT (padded[0], WithinAbs (3.0f, 1.0e-6f));
    REQUIRE_THAT (padded[1], WithinAbs (2.0f, 1.0e-6f));
    REQUIRE_THAT (padded[2], WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (padded[5], WithinAbs (4.0f, 1.0e-6f));
    REQUIRE_THAT (padded[6], WithinAbs (3.0f, 1.0e-6f));
    REQUIRE_THAT (padded[7], WithinAbs (2.0f, 1.0e-6f));
}
