/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace opendj::stemdsp
{

/** Numbers dictated by the htdemucs model, not chosen here. The converted
    OpenVINO graph has fixed shapes, so every one of these is load bearing. */
constexpr int sampleRate   = 44100;
constexpr int segment      = 343980;          ///< 7.8 s, the model's time input
constexpr int nFft         = 4096;
constexpr int hop          = 1024;
constexpr int freqBins     = nFft / 2;        ///< 2048: the Nyquist bin is dropped
constexpr int frames       = (segment + hop - 1) / hop;   ///< 336
constexpr int numSources   = 4;
constexpr int numChannels  = 2;

/** A one-sided spectrum, laid out as separate real and imaginary planes of
    bins x frames, which is the order the model wants and the order the
    reference numpy uses. */
struct Spectrogram
{
    int bins = 0;
    int frames = 0;
    std::vector<float> real;
    std::vector<float> imaginary;

    void resize (int numBins, int numFrames)
    {
        bins = numBins;
        frames = numFrames;
        real.assign ((size_t) (numBins * numFrames), 0.0f);
        imaginary.assign ((size_t) (numBins * numFrames), 0.0f);
    }

    float& re (int bin, int frame) noexcept       { return real[(size_t) (bin * frames + frame)]; }
    float& im (int bin, int frame) noexcept       { return imaginary[(size_t) (bin * frames + frame)]; }
    float re (int bin, int frame) const noexcept  { return real[(size_t) (bin * frames + frame)]; }
    float im (int bin, int frame) const noexcept  { return imaginary[(size_t) (bin * frames + frame)]; }
};

/** torch.hann_window(n), which is the periodic window: 0.5 - 0.5 cos(2 pi k/n).
    Not the symmetric one juce::dsp::WindowingFunction would give. */
std::vector<float> hannPeriodic (int n);

/** torch.stft with the flags htdemucs uses: periodic Hann, center, reflect
    padding, normalised, one sided. Returns bins x frames. */
Spectrogram stft (const float* signal, int length, int fftSize = nFft, int hopSize = hop);

/** torch.istft with the matching flags, trimmed to `length`. */
std::vector<float> istft (const Spectrogram& z, int length, int fftSize = nFft, int hopSize = hop);

/** demucs.hdemucs.pad1d: reflect padding, but zero padding first when the
    signal is shorter than the padding asked for, which reflect cannot do. */
std::vector<float> pad1d (const float* signal, int length, int left, int right);

/** HTDemucs._spec: the stft above, minus the Nyquist bin and the two frames of
    padding at each end. */
Spectrogram spec (const float* signal, int length);

/** HTDemucs._ispec, the inverse of spec(). */
std::vector<float> ispec (const Spectrogram& z, int length);

} // namespace opendj::stemdsp
