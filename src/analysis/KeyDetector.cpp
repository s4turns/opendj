/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/KeyDetector.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace opendj
{

namespace
{
    constexpr int fftOrder = 13;            // 8192 points
    constexpr int fftSize = 1 << fftOrder;

    // No overlap. Key is a property of a whole track, not of any one moment, so
    // there is nothing to be gained from looking at the same bar four times.
    constexpr int hopSize = fftSize;

    const char* const sharpNames[] = { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };

    /** Krumhansl and Kessler's profiles: how strongly each degree of the scale
        is felt in a key. Correlating a chromagram against all twenty-four
        rotations of these is the oldest method there is and still a good one. */
    constexpr std::array<float, 12> majorProfile
    {
        6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
        2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
    };

    constexpr std::array<float, 12> minorProfile
    {
        6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
        2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
    };

    float mean (const std::array<float, 12>& values)
    {
        return std::accumulate (values.begin(), values.end(), 0.0f) / 12.0f;
    }

    /** Pearson correlation between a chromagram and a profile rotated to start
        on `tonic`. Correlation rather than a dot product, so a track that simply
        has more energy everywhere does not score higher. */
    float correlate (const std::array<float, 12>& chroma,
                     const std::array<float, 12>& profile,
                     int tonic)
    {
        const auto chromaMean = mean (chroma);
        const auto profileMean = mean (profile);

        auto covariance = 0.0f;
        auto chromaVariance = 0.0f;
        auto profileVariance = 0.0f;

        for (int i = 0; i < 12; ++i)
        {
            const auto c = chroma[(size_t) i] - chromaMean;
            const auto p = profile[(size_t) ((i - tonic + 12) % 12)] - profileMean;

            covariance += c * p;
            chromaVariance += c * c;
            profileVariance += p * p;
        }

        const auto denominator = std::sqrt (chromaVariance * profileVariance);
        return denominator > 1.0e-9f ? covariance / denominator : 0.0f;
    }

    std::vector<float> toMono (const juce::AudioBuffer<float>& audio, int start, int length)
    {
        const auto numChannels = juce::jmax (1, audio.getNumChannels());
        std::vector<float> mono ((size_t) length, 0.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* source = audio.getReadPointer (ch) + start;

            for (int i = 0; i < length; ++i)
                mono[(size_t) i] += source[i];
        }

        const auto scale = 1.0f / static_cast<float> (numChannels);

        for (auto& sample : mono)
            sample *= scale;

        return mono;
    }
}

//==============================================================================
// Naming
//==============================================================================

juce::String MusicalKey::toString() const
{
    if (! isValid())
        return {};

    return juce::String (sharpNames[tonic]) + (mode == Mode::minor ? "m" : "");
}

juce::String MusicalKey::toCamelot() const
{
    if (! isValid())
        return {};

    // The wheel is the circle of fifths with minor keys on the inside. A key and
    // its relative minor share a number, which is why 8A and 8B mix.
    const auto offset = mode == Mode::minor ? 4 : 7;
    const auto number = ((tonic * 7 + offset) % 12) + 1;

    return juce::String (number) + (mode == Mode::minor ? "A" : "B");
}

MusicalKey MusicalKey::fromString (const juce::String& text)
{
    const auto trimmed = text.trim();

    if (trimmed.isEmpty())
        return {};

    // Camelot first: a number followed by A or B.
    if (const auto last = trimmed.getLastCharacter();
        (last == 'A' || last == 'a' || last == 'B' || last == 'b')
        && trimmed.dropLastCharacters (1).containsOnly ("0123456789"))
    {
        const auto number = trimmed.dropLastCharacters (1).getIntValue();

        if (number >= 1 && number <= 12)
        {
            MusicalKey key;
            key.mode = (last == 'A' || last == 'a') ? Mode::minor : Mode::major;

            // Undo the wheel: 7 steps of a fifth is a semitone, so multiply by
            // its inverse modulo twelve, which is 7 again.
            const auto offset = key.mode == Mode::minor ? 4 : 7;
            key.tonic = (((number - 1) - offset) * 7 % 12 + 12) % 12;
            return key;
        }
    }

    // Otherwise a note name, optionally flat or sharp, optionally minor.
    auto body = trimmed;
    auto mode = Mode::major;

    for (const auto* suffix : { "minor", "min", "m" })
    {
        if (body.endsWithIgnoreCase (suffix))
        {
            body = body.dropLastCharacters ((int) juce::String (suffix).length()).trim();
            mode = Mode::minor;
            break;
        }
    }

    // What is left has to be a note and at most one accidental, or it is not a
    // key at all. Without this length check "banana" reads as B major.
    if (body.isEmpty() || body.length() > 2)
        return {};

    const auto letter = juce::String::charToString (body[0]).toUpperCase();
    const auto accidental = body.length() > 1 ? body[1] : ' ';

    if (body.length() > 1 && accidental != '#' && accidental != 'b')
        return {};

    static const std::pair<const char*, int> naturals[]
    {
        { "C", 0 }, { "D", 2 }, { "E", 4 }, { "F", 5 },
        { "G", 7 }, { "A", 9 }, { "B", 11 }
    };

    for (const auto& [name, pitch] : naturals)
    {
        if (letter != name)
            continue;

        auto tonic = pitch;

        if (accidental == '#')       tonic = (tonic + 1) % 12;
        else if (accidental == 'b')  tonic = (tonic + 11) % 12;

        MusicalKey key;
        key.tonic = tonic;
        key.mode = mode;
        return key;
    }

    return {};
}

//==============================================================================
// Detection
//==============================================================================

std::array<float, 12> KeyDetector::chromagram (const juce::AudioBuffer<float>& audio,
                                               double sampleRate,
                                               Options options)
{
    std::array<float, 12> chroma {};
    chroma.fill (0.0f);

    const auto totalSamples = audio.getNumSamples();

    if (totalSamples < fftSize || sampleRate <= 0.0)
        return chroma;

    // Take the window from the middle, where the music is rather than the drums.
    const auto wanted = juce::jmin (totalSamples,
                                    static_cast<int> (options.secondsToExamine * sampleRate));
    const auto start = (totalSamples - wanted) / 2;
    const auto mono = toMono (audio, start, wanted);

    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize,
                                                juce::dsp::WindowingFunction<float>::hann);

    std::vector<float> frame ((size_t) fftSize * 2, 0.0f);

    const auto lowestBin = juce::jmax (1, static_cast<int> (options.lowestHz * fftSize / sampleRate));
    const auto highestBin = juce::jmin (fftSize / 2 - 1,
                                        static_cast<int> (options.highestHz * fftSize / sampleRate));

    auto framesUsed = 0;

    for (int offset = 0; offset + fftSize <= static_cast<int> (mono.size()); offset += hopSize)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        std::copy (mono.begin() + offset, mono.begin() + offset + fftSize, frame.begin());

        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data());

        std::array<float, 12> frameChroma {};
        frameChroma.fill (0.0f);

        for (int bin = lowestBin; bin <= highestBin; ++bin)
        {
            const auto frequency = bin * sampleRate / fftSize;

            // Where this bin falls on the piano, as a fractional semitone.
            const auto semitone = 69.0 + 12.0 * std::log2 (frequency / 440.0);
            const auto nearest = std::round (semitone);
            const auto centsOff = std::abs (semitone - nearest) * 100.0;

            // A bin sitting between two semitones belongs to neither. Letting it
            // vote anyway is what turns a chromagram into a flat smear.
            if (centsOff > 35.0)
                continue;

            const auto pitchClass = (static_cast<int> (nearest) % 12 + 12) % 12;
            frameChroma[(size_t) pitchClass] += frame[(size_t) bin];
        }

        // Normalise every frame before adding it in, so a loud drop does not
        // outvote the four quiet minutes that share its key.
        const auto total = std::accumulate (frameChroma.begin(), frameChroma.end(), 0.0f);

        if (total <= 1.0e-9f)
            continue;

        for (size_t i = 0; i < 12; ++i)
            chroma[i] += frameChroma[i] / total;

        ++framesUsed;
    }

    if (framesUsed == 0)
        return chroma;

    for (auto& value : chroma)
        value /= static_cast<float> (framesUsed);

    return chroma;
}

MusicalKey KeyDetector::fromChromagram (const std::array<float, 12>& chroma)
{
    if (std::accumulate (chroma.begin(), chroma.end(), 0.0f) <= 0.0f)
        return {};

    MusicalKey best;
    auto bestScore = -2.0f;
    auto runnerUp = -2.0f;

    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (const auto mode : { MusicalKey::Mode::major, MusicalKey::Mode::minor })
        {
            const auto& profile = mode == MusicalKey::Mode::major ? majorProfile : minorProfile;
            const auto score = correlate (chroma, profile, tonic);

            if (score > bestScore)
            {
                runnerUp = bestScore;
                bestScore = score;
                best.tonic = tonic;
                best.mode = mode;
            }
            else if (score > runnerUp)
            {
                runnerUp = score;
            }
        }
    }

    if (bestScore <= 0.0f)
        return {};

    // How far clear the winner finished. A quarter of a correlation point is a
    // comfortable margin, so anything at or past that reads as full confidence.
    best.confidence = juce::jlimit (0.0f, 1.0f, (bestScore - runnerUp) / 0.25f);
    return best;
}

MusicalKey KeyDetector::detect (const juce::AudioBuffer<float>& audio,
                                double sampleRate,
                                Options options)
{
    return fromChromagram (chromagram (audio, sampleRate, options));
}

} // namespace opendj
