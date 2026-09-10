/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemDsp.h"

#include <cmath>

namespace opendj::stemdsp
{

namespace
{
    /** Reflect an index back inside [0, length), the way numpy and torch do:
        the edge sample itself is not repeated. */
    inline int reflect (int index, int length) noexcept
    {
        if (length <= 1)
            return 0;

        const auto period = 2 * (length - 1);

        index = std::abs (index) % period;
        return index < length ? index : period - index;
    }

    juce::dsp::FFT& fftOfSize (int fftSize)
    {
        // One transform per size, built once. The audio thread never comes here.
        static juce::dsp::FFT fft (static_cast<int> (std::log2 (nFft)));
        jassert (fftSize == nFft);
        juce::ignoreUnused (fftSize);
        return fft;
    }
}

std::vector<float> hannPeriodic (int n)
{
    std::vector<float> window ((size_t) n);

    for (int k = 0; k < n; ++k)
        window[(size_t) k] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * k / n));

    return window;
}

Spectrogram stft (const float* signal, int length, int fftSize, int hopSize)
{
    const auto half = fftSize / 2;
    const auto padded = length + 2 * half;
    const auto numFrames = padded >= fftSize ? (padded - fftSize) / hopSize + 1 : 0;
    const auto bins = fftSize / 2 + 1;

    Spectrogram out;
    out.resize (bins, numFrames);

    if (numFrames <= 0)
        return out;

    const auto window = hannPeriodic (fftSize);
    const auto scale = 1.0f / std::sqrt ((float) fftSize);

    auto& fft = fftOfSize (fftSize);
    std::vector<float> frame ((size_t) (2 * fftSize));

    for (int f = 0; f < numFrames; ++f)
    {
        // The frame is read straight out of the signal with reflected indices,
        // rather than materialising a padded copy of the whole track.
        const auto start = f * hopSize - half;

        for (int i = 0; i < fftSize; ++i)
            frame[(size_t) i] = signal[reflect (start + i, length)] * window[(size_t) i];

        std::fill (frame.begin() + fftSize, frame.end(), 0.0f);
        fft.performRealOnlyForwardTransform (frame.data(), true);

        for (int bin = 0; bin < bins; ++bin)
        {
            out.re (bin, f) = frame[(size_t) (2 * bin)] * scale;
            out.im (bin, f) = frame[(size_t) (2 * bin + 1)] * scale;
        }
    }

    return out;
}

std::vector<float> istft (const Spectrogram& z, int length, int fftSize, int hopSize)
{
    const auto numFrames = z.frames;
    const auto total = numFrames > 0 ? (numFrames - 1) * hopSize + fftSize : 0;
    const auto half = fftSize / 2;

    std::vector<double> sum ((size_t) juce::jmax (0, total), 0.0);
    std::vector<double> envelope ((size_t) juce::jmax (0, total), 0.0);

    if (numFrames > 0)
    {
        const auto window = hannPeriodic (fftSize);
        const auto scale = std::sqrt ((float) fftSize);

        auto& fft = fftOfSize (fftSize);
        std::vector<float> frame ((size_t) (2 * fftSize));

        for (int f = 0; f < numFrames; ++f)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);

            for (int bin = 0; bin < z.bins; ++bin)
            {
                frame[(size_t) (2 * bin)] = z.re (bin, f);
                frame[(size_t) (2 * bin + 1)] = z.im (bin, f);
            }

            fft.performRealOnlyInverseTransform (frame.data());

            const auto offset = f * hopSize;

            for (int i = 0; i < fftSize; ++i)
            {
                const auto w = window[(size_t) i];
                sum[(size_t) (offset + i)] += (double) frame[(size_t) i] * w * scale;
                envelope[(size_t) (offset + i)] += (double) w * w;
            }
        }
    }

    // torch trims the centre padding and divides out the overlap-add envelope,
    // then zero-pads if the caller asked for more than there is.
    std::vector<float> out ((size_t) length, 0.0f);
    const auto available = juce::jmin (length, juce::jmax (0, total - half));

    for (int i = 0; i < available; ++i)
    {
        const auto e = envelope[(size_t) (half + i)];
        out[(size_t) i] = e > 1.0e-11 ? (float) (sum[(size_t) (half + i)] / e) : 0.0f;
    }

    return out;
}

std::vector<float> pad1d (const float* signal, int length, int left, int right)
{
    // Reflect padding cannot reach further than the signal is long, so a short
    // signal is zero-extended first and the extension removed afterwards.
    const auto maxPad = juce::jmax (left, right);
    const auto extra = length <= maxPad ? maxPad - length + 1 : 0;

    std::vector<float> source ((size_t) (length + extra), 0.0f);
    std::copy (signal, signal + length, source.begin());

    const auto sourceLength = (int) source.size();
    std::vector<float> out ((size_t) (sourceLength + left + right));

    for (int i = 0; i < (int) out.size(); ++i)
        out[(size_t) i] = source[(size_t) reflect (i - left, sourceLength)];

    if (extra > 0)
        out.resize (out.size() - (size_t) extra);

    return out;
}

Spectrogram spec (const float* signal, int length)
{
    const auto le = (length + hop - 1) / hop;
    const auto pad = hop / 2 * 3;

    const auto padded = pad1d (signal, length, pad, pad + le * hop - length);
    const auto full = stft (padded.data(), (int) padded.size());

    // Drop the Nyquist bin and the two frames of padding at each end.
    Spectrogram out;
    out.resize (freqBins, le);

    for (int bin = 0; bin < freqBins; ++bin)
    {
        for (int f = 0; f < le; ++f)
        {
            out.re (bin, f) = full.re (bin, f + 2);
            out.im (bin, f) = full.im (bin, f + 2);
        }
    }

    return out;
}

std::vector<float> ispec (const Spectrogram& z, int length)
{
    const auto pad = hop / 2 * 3;

    // Put back the zero Nyquist bin and the two zero frames at each end.
    Spectrogram padded;
    padded.resize (z.bins + 1, z.frames + 4);

    for (int bin = 0; bin < z.bins; ++bin)
    {
        for (int f = 0; f < z.frames; ++f)
        {
            padded.re (bin, f + 2) = z.re (bin, f);
            padded.im (bin, f + 2) = z.im (bin, f);
        }
    }

    const auto le = hop * ((length + hop - 1) / hop) + 2 * pad;
    const auto wave = istft (padded, le, 2 * (padded.bins - 1), hop);

    std::vector<float> out ((size_t) length, 0.0f);

    for (int i = 0; i < length && pad + i < (int) wave.size(); ++i)
        out[(size_t) i] = wave[(size_t) (pad + i)];

    return out;
}

} // namespace opendj::stemdsp
