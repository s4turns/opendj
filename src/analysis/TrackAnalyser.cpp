/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/TrackAnalyser.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace opendj
{

namespace
{
    constexpr int fftOrder = 10;              // 1024 point frames
    constexpr int fftSize = 1 << fftOrder;
    constexpr int hopSize = fftSize / 4;      // 75 percent overlap

    /** Sums the channels down to mono. Beat detection has no use for the stereo
        image, and one channel halves the work. */
    std::vector<float> toMono (const juce::AudioBuffer<float>& audio)
    {
        const auto numSamples = audio.getNumSamples();
        const auto numChannels = juce::jmax (1, audio.getNumChannels());

        std::vector<float> mono (static_cast<size_t> (numSamples), 0.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* source = audio.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                mono[(size_t) i] += source[i];
        }

        const auto scale = 1.0f / static_cast<float> (numChannels);

        for (auto& sample : mono)
            sample *= scale;

        return mono;
    }

    struct Envelopes
    {
        std::vector<float> full;   ///< the whole spectrum, for finding the tempo
        std::vector<float> low;    ///< bass only, for finding where the beat falls
    };

    /** Spectral flux: how much energy rose since the previous frame, summed
        across the spectrum. Peaks in this line up with note and drum onsets.

        Two envelopes come out of the same pass. The full band one is the better
        tempo estimate because it sees every rhythmic layer. Phase, though, wants
        the bass alone: with a kick on the beat and a hat between them, a full
        band envelope is just as happy to lock on to the offbeat, and half the
        library would come out with its grid shifted by an eighth note.
    */
    Envelopes onsetEnvelopes (const std::vector<float>& mono, double sampleRate)
    {
        if (mono.size() < static_cast<size_t> (fftSize))
            return {};

        // Everything below roughly 200 Hz, which is kick and bass territory.
        const auto lowBinLimit = juce::jlimit (2, fftSize / 2,
                                               static_cast<int> (200.0 * fftSize / sampleRate));

        juce::dsp::FFT fft (fftOrder);
        juce::dsp::WindowingFunction<float> window (static_cast<size_t> (fftSize),
                                                    juce::dsp::WindowingFunction<float>::hann);

        const auto numFrames = (static_cast<int> (mono.size()) - fftSize) / hopSize + 1;

        std::vector<float> envelope, lowEnvelope;
        envelope.reserve (static_cast<size_t> (numFrames));
        lowEnvelope.reserve (static_cast<size_t> (numFrames));

        std::vector<float> frame (static_cast<size_t> (fftSize * 2), 0.0f);
        std::vector<float> magnitudes (static_cast<size_t> (fftSize / 2), 0.0f);
        std::vector<float> previous (static_cast<size_t> (fftSize / 2), 0.0f);

        for (int f = 0; f < numFrames; ++f)
        {
            const auto offset = static_cast<size_t> (f) * hopSize;

            std::fill (frame.begin(), frame.end(), 0.0f);
            std::copy (mono.begin() + static_cast<long> (offset),
                       mono.begin() + static_cast<long> (offset + fftSize),
                       frame.begin());

            window.multiplyWithWindowingTable (frame.data(), static_cast<size_t> (fftSize));
            fft.performFrequencyOnlyForwardTransform (frame.data());

            float flux = 0.0f;
            float lowFlux = 0.0f;

            for (int bin = 0; bin < fftSize / 2; ++bin)
            {
                // Compress the magnitudes, so a loud bass drum does not drown
                // out the hats that often carry the clearer beat.
                const auto magnitude = std::log1p (frame[(size_t) bin] * 10.0f);
                magnitudes[(size_t) bin] = magnitude;

                const auto rise = juce::jmax (0.0f, magnitude - previous[(size_t) bin]);
                flux += rise;

                if (bin < lowBinLimit)
                    lowFlux += rise;
            }

            previous.swap (magnitudes);
            envelope.push_back (flux);
            lowEnvelope.push_back (lowFlux);
        }

        // Subtract a local average and rectify, which removes the slow drift that
        // would otherwise dominate the autocorrelation.
        const auto removeDrift = [] (const std::vector<float>& source)
        {
            const int windowFrames = 16;
            std::vector<float> rectified (source.size(), 0.0f);

            for (size_t i = 0; i < source.size(); ++i)
            {
                const auto first = i > static_cast<size_t> (windowFrames) ? i - windowFrames : 0u;
                const auto last = juce::jmin (source.size(), i + windowFrames + 1);

                const auto sum = std::accumulate (source.begin() + static_cast<long> (first),
                                                  source.begin() + static_cast<long> (last),
                                                  0.0f);
                const auto mean = sum / static_cast<float> (last - first);

                rectified[i] = juce::jmax (0.0f, source[i] - mean);
            }

            return rectified;
        };

        return { removeDrift (envelope), removeDrift (lowEnvelope) };
    }

    struct TempoEstimate
    {
        double bpm = 0.0;
        double periodFrames = 0.0;
        float confidence = 0.0f;
    };

    TempoEstimate estimateTempo (const std::vector<float>& envelope,
                                 double framesPerSecond,
                                 double minimumBpm,
                                 double maximumBpm)
    {
        if (envelope.size() < 64)
            return {};

        const auto lagForBpm = [framesPerSecond] (double bpm) { return 60.0 * framesPerSecond / bpm; };

        const auto shortestLag = juce::jmax (2, static_cast<int> (std::floor (lagForBpm (maximumBpm))));
        const auto longestLag = juce::jmin (static_cast<int> (envelope.size() / 2),
                                            static_cast<int> (std::ceil (lagForBpm (minimumBpm))));

        if (longestLag <= shortestLag)
            return {};

        std::vector<float> correlation (static_cast<size_t> (longestLag + 1), 0.0f);

        for (int lag = shortestLag; lag <= longestLag; ++lag)
        {
            float sum = 0.0f;

            for (size_t i = 0; i + static_cast<size_t> (lag) < envelope.size(); ++i)
                sum += envelope[i] * envelope[i + static_cast<size_t> (lag)];

            correlation[(size_t) lag] = sum / static_cast<float> (envelope.size() - lag);
        }

        // Music is periodic at the bar as well as the beat, so the raw peak is
        // often at two or four times the beat. Adding in the harmonics rewards
        // the lag that also explains its own multiples.
        std::vector<float> scored = correlation;

        for (int lag = shortestLag; lag <= longestLag; ++lag)
        {
            for (const int multiple : { 2, 4 })
            {
                const auto harmonic = lag * multiple;

                if (harmonic <= longestLag)
                    scored[(size_t) lag] += correlation[(size_t) harmonic] * 0.5f;
            }
        }

        const auto best = std::max_element (scored.begin() + shortestLag,
                                            scored.begin() + longestLag + 1);

        if (best == scored.begin() + longestLag + 1 || *best <= 0.0f)
            return {};

        const auto bestLag = static_cast<int> (std::distance (scored.begin(), best));

        // Refine the peak with a parabola through its neighbours, which recovers
        // the fraction of a frame that integer lags throw away.
        auto refinedLag = static_cast<double> (bestLag);

        if (bestLag > shortestLag && bestLag < longestLag)
        {
            const auto left = scored[(size_t) bestLag - 1];
            const auto centre = scored[(size_t) bestLag];
            const auto right = scored[(size_t) bestLag + 1];
            const auto denominator = left - 2.0f * centre + right;

            if (std::abs (denominator) > 1.0e-9f)
                refinedLag += 0.5 * (left - right) / denominator;
        }

        const auto mean = std::accumulate (scored.begin() + shortestLag,
                                           scored.begin() + longestLag + 1, 0.0f)
                        / static_cast<float> (longestLag - shortestLag + 1);

        TempoEstimate estimate;
        estimate.periodFrames = refinedLag;
        estimate.bpm = 60.0 * framesPerSecond / refinedLag;
        estimate.confidence = mean > 0.0f ? juce::jlimit (0.0f, 1.0f, (*best / mean - 1.0f) * 0.25f) : 0.0f;

        return estimate;
    }

    /** Slides a pulse train across the envelope to find where the beats sit. */
    double findFirstBeat (const std::vector<float>& envelope, double periodFrames, double framesPerSecond)
    {
        if (periodFrames < 2.0 || envelope.empty())
            return 0.0;

        const auto period = static_cast<int> (std::round (periodFrames));

        int bestOffset = 0;
        float bestScore = -1.0f;

        for (int offset = 0; offset < period; ++offset)
        {
            float score = 0.0f;

            for (double frame = offset; frame < static_cast<double> (envelope.size()); frame += periodFrames)
                score += envelope[static_cast<size_t> (frame)];

            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = offset;
            }
        }

        // A frame is timestamped by where its window starts, but the Hann window
        // weights the middle, so flux peaks about half a window before the onset
        // it is reporting. Push the anchor forward by that much, then fold it
        // back into the first beat period.
        const auto halfWindowSeconds = (fftSize / 2.0) / (framesPerSecond * hopSize);
        const auto periodSeconds = periodFrames / framesPerSecond;

        auto anchor = bestOffset / framesPerSecond + halfWindowSeconds;

        while (anchor >= periodSeconds)
            anchor -= periodSeconds;

        return anchor;
    }
}

WaveformPeaks TrackAnalyser::buildPeaks (const juce::AudioBuffer<float>& audio, int samplesPerBucket)
{
    WaveformPeaks peaks;
    peaks.samplesPerBucket = juce::jmax (1, samplesPerBucket);

    const auto numSamples = audio.getNumSamples();
    const auto numChannels = juce::jmax (1, audio.getNumChannels());

    if (numSamples <= 0)
        return peaks;

    const auto numBuckets = (numSamples + peaks.samplesPerBucket - 1) / peaks.samplesPerBucket;
    peaks.buckets.resize (static_cast<size_t> (numBuckets));

    for (int b = 0; b < numBuckets; ++b)
    {
        const auto start = b * peaks.samplesPerBucket;
        const auto length = juce::jmin (peaks.samplesPerBucket, numSamples - start);

        auto& bucket = peaks.buckets[(size_t) b];
        bucket.minimum = 0.0f;
        bucket.maximum = 0.0f;

        double sumOfSquares = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* source = audio.getReadPointer (ch) + start;

            for (int i = 0; i < length; ++i)
            {
                const auto sample = source[i];
                bucket.minimum = juce::jmin (bucket.minimum, sample);
                bucket.maximum = juce::jmax (bucket.maximum, sample);
                sumOfSquares += static_cast<double> (sample) * sample;
            }
        }

        const auto count = juce::jmax (1, length * numChannels);
        bucket.energy = static_cast<float> (std::sqrt (sumOfSquares / count));
    }

    return peaks;
}

std::shared_ptr<const TrackAnalysis> TrackAnalyser::analyse (const juce::AudioBuffer<float>& audio,
                                                             double sampleRate,
                                                             Options options)
{
    auto analysis = std::make_shared<TrackAnalysis>();
    analysis->sampleRate = sampleRate;

    const auto numSamples = audio.getNumSamples();

    if (numSamples <= 0 || sampleRate <= 0.0)
        return analysis;

    const auto overviewBucket = juce::jmax (1, numSamples / juce::jmax (1, options.overviewBuckets));
    const auto detailBucket = juce::jmax (1, static_cast<int> (sampleRate * options.detailBucketMs / 1000.0));

    analysis->overview = buildPeaks (audio, overviewBucket);
    analysis->detail = buildPeaks (audio, detailBucket);

    const auto envelopes = onsetEnvelopes (toMono (audio), sampleRate);
    const auto& envelope = envelopes.full;

    if (envelope.empty())
        return analysis;

    const auto framesPerSecond = sampleRate / hopSize;
    const auto tempo = estimateTempo (envelope, framesPerSecond, options.minimumBpm, options.maximumBpm);

    if (tempo.bpm <= 0.0)
        return analysis;

    analysis->bpm = tempo.bpm;
    analysis->confidence = tempo.confidence;
    // Phase comes off the bass envelope where there is one; a track with no
    // low end at all falls back to the full band rather than giving up.
    const auto lowEnergy = std::accumulate (envelopes.low.begin(), envelopes.low.end(), 0.0f);
    const auto& phaseEnvelope = lowEnergy > 0.0f ? envelopes.low : envelope;

    analysis->firstBeatSeconds = findFirstBeat (phaseEnvelope, tempo.periodFrames, framesPerSecond);

    return analysis;
}

} // namespace opendj
