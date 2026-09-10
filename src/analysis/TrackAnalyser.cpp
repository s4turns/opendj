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

    /** How well a train of pulses at this spacing explains the envelope, at its
        best alignment: the mean envelope value under a pulse.

        This is the measurement that separates a real tempo from a metrical
        impostor. Autocorrelation at three quarters of the beat is contaminated
        by the beat itself, so a 128 BPM track scores well at 170.7 too; a pulse
        train at 170.7 lands on a beat only once every four pulses, and the mean
        falls to a quarter. Dividing by the number of pulses rather than summing
        is what makes periods of different lengths comparable.
    */
    struct PulseFit
    {
        int offset = 0;
        float mean = 0.0f;

        /** The level most pulses reach, rather than the level they average.

            This is the number that tells a tempo from a metrical impostor. A
            train at four thirds of the true tempo lands on a beat only once
            every four pulses, and a train at two thirds alternates between the
            beat and the offbeat; both can average well, because the pulses that
            do land land on something loud. Asking instead what the weakest
            quarter of the pulses found makes a train that misses most of the
            time score badly however loud its hits are.
        */
        float strength = 0.0f;
    };

    /** The envelope at a pulse, allowing for a beat that falls between frames. */
    float sampleAround (const std::vector<float>& envelope, double frame)
    {
        const auto centre = static_cast<size_t> (frame);
        auto value = envelope[centre];

        if (centre > 0)
            value = juce::jmax (value, envelope[centre - 1]);

        if (centre + 1 < envelope.size())
            value = juce::jmax (value, envelope[centre + 1]);

        return value;
    }

    PulseFit fitPulseTrain (const std::vector<float>& envelope, double periodFrames)
    {
        PulseFit fit;

        if (periodFrames < 2.0 || envelope.empty())
            return fit;

        const auto period = static_cast<int> (std::round (periodFrames));
        const auto lastFrame = static_cast<double> (envelope.size());

        // The train is aligned in windows of a few bars rather than once for the
        // whole track. A rigid grid laid over five minutes drifts off the beat
        // long before the end, from the fraction of a frame the period is out
        // by and from the track's own timing, and a drifting grid scores a
        // correct tempo as a miss.
        const auto windowFrames = periodFrames * 8.0;

        std::vector<float> hits;
        auto sum = 0.0f;

        for (double windowStart = 0.0; windowStart < lastFrame; windowStart += windowFrames)
        {
            const auto windowEnd = juce::jmin (lastFrame, windowStart + windowFrames);

            // A window with barely a beat in it says nothing; leave it out
            // rather than let it vote.
            if (windowEnd - windowStart < periodFrames * 2.0)
                break;

            auto bestOffset = 0;
            auto bestMean = -1.0f;

            for (int offset = 0; offset < period; ++offset)
            {
                auto windowSum = 0.0f;
                auto count = 0;

                for (double frame = windowStart + offset; frame < windowEnd; frame += periodFrames)
                {
                    windowSum += sampleAround (envelope, frame);
                    ++count;
                }

                if (const auto mean = count > 0 ? windowSum / static_cast<float> (count) : 0.0f;
                    mean > bestMean)
                {
                    bestMean = mean;
                    bestOffset = offset;
                }
            }

            // The first window starts at zero, so its offset is the phase of the
            // whole grid and is what the beat anchor is taken from.
            if (hits.empty())
                fit.offset = bestOffset;

            for (double frame = windowStart + bestOffset; frame < windowEnd; frame += periodFrames)
            {
                const auto value = sampleAround (envelope, frame);
                hits.push_back (value);
                sum += value;
            }
        }

        if (hits.empty())
            return fit;

        fit.mean = sum / static_cast<float> (hits.size());

        // A quarter of the way up the sorted pulses: low enough that a
        // breakdown or a dropped beat does not condemn a correct tempo, high
        // enough that a train missing three pulses in four cannot hide.
        const auto quarter = hits.size() / 4;
        std::nth_element (hits.begin(), hits.begin() + static_cast<long> (quarter), hits.end());
        fit.strength = hits[quarter];

        return fit;
    }

    /** How likely a tempo is before anything has been listened to.

        Autocorrelation cannot tell a tempo from half or twice it: a pulse train
        at half the tempo lands on every second beat, and every one of those is
        still a beat. Only a prior breaks that tie, so this is where the
        assumption lives, in one line, rather than being spread through the
        search. Centred on 130 and wide enough that 90 and 175 are both
        comfortably reachable.
    */
    double tempoPrior (double bpm)
    {
        constexpr double centreBpm = 130.0;
        constexpr double width = 0.7;   // octaves

        const auto octaves = std::log2 (bpm / centreBpm) / width;
        return std::exp (-0.5 * octaves * octaves);
    }

    /** The mean of an envelope, as the scale its pulse scores are measured in. */
    float meanOf (const std::vector<float>& envelope)
    {
        if (envelope.empty())
            return 0.0f;

        return std::accumulate (envelope.begin(), envelope.end(), 0.0f)
             / static_cast<float> (envelope.size());
    }

    TempoEstimate estimateTempo (const std::vector<float>& envelope,
                                 const std::vector<float>& lowEnvelope,
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

        // Correlate well past the slowest tempo on offer, so that a candidate at
        // the fast end still has its own multiples to be judged against. With
        // the window stopping at the slowest tempo, a 128 BPM peak had neither
        // of its harmonics in range while its impostors did.
        const auto correlationLimit = juce::jmin (static_cast<int> (envelope.size() / 2),
                                                  longestLag * 4);

        std::vector<float> correlation (static_cast<size_t> (correlationLimit + 1), 0.0f);

        for (int lag = shortestLag; lag <= correlationLimit; ++lag)
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

                if (harmonic <= correlationLimit)
                    scored[(size_t) lag] += correlation[(size_t) harmonic] * 0.5f;
            }
        }

        // Every lag that beats both its neighbours is worth considering, rather
        // than only the single highest: the tallest peak is regularly a
        // multiple or a fraction of the tempo a listener would tap.
        std::vector<int> candidates;

        for (int lag = shortestLag + 1; lag < longestLag; ++lag)
            if (scored[(size_t) lag] > scored[(size_t) lag - 1]
                && scored[(size_t) lag] >= scored[(size_t) lag + 1]
                && scored[(size_t) lag] > 0.0f)
                candidates.push_back (lag);

        if (candidates.empty())
            return {};

        std::sort (candidates.begin(), candidates.end(),
                   [&scored] (int a, int b) { return scored[(size_t) a] > scored[(size_t) b]; });

        if (candidates.size() > 12)
            candidates.resize (12);

        // Each peak brings its metrical relatives along, so that the right
        // answer is on the list even when it never showed up as a peak of its
        // own. The thirds are what a triplet feel or a 4/3 error hides behind.
        std::vector<int> toTest;

        for (const auto lag : candidates)
        {
            for (const double ratio : { 0.5, 2.0 / 3.0, 0.75, 1.0, 4.0 / 3.0, 1.5, 2.0 })
            {
                const auto related = static_cast<int> (std::round (lag * ratio));

                if (related >= shortestLag && related <= longestLag)
                    toTest.push_back (related);
            }
        }

        std::sort (toTest.begin(), toTest.end());
        toTest.erase (std::unique (toTest.begin(), toTest.end()), toTest.end());

        // A whole number of frames is too coarse to lay a grid with: at 128 BPM
        // the beat is 80.75 frames, and rounding that to 81 walks ten frames off
        // the beat over a single track. Interpolating the correlation peak
        // recovers the fraction before anything is measured with it.
        const auto refine = [&scored, shortestLag, longestLag] (int lag)
        {
            if (lag <= shortestLag || lag >= longestLag)
                return static_cast<double> (lag);

            const auto left = scored[(size_t) lag - 1];
            const auto centre = scored[(size_t) lag];
            const auto right = scored[(size_t) lag + 1];
            const auto denominator = left - 2.0f * centre + right;

            if (denominator >= -1.0e-9f)
                return static_cast<double> (lag);

            return lag + juce::jlimit (-0.5, 0.5, 0.5 * (double) (left - right) / denominator);
        };

        // How far a pulse train stands out from the envelope it is laid over.
        // Dividing by the envelope's own mean makes the two bands comparable,
        // so they can be added rather than arbitrated between.
        const auto fullScale = meanOf (envelope);
        const auto lowScale = meanOf (lowEnvelope);
        const auto useLowBand = lowScale > 0.0f;

        const auto scoreFor = [&] (double lag)
        {
            const auto bpm = 60.0 * framesPerSecond / lag;

            auto contrast = fullScale > 0.0f ? fitPulseTrain (envelope, lag).strength / fullScale : 0.0f;

            // The kick is what a dancer hears as the beat, so the low band gets
            // the louder vote. Judged on the bass alone a track without any is
            // lost, so the full spectrum still has a say.
            if (useLowBand)
                contrast = contrast + 2.0f * (fitPulseTrain (lowEnvelope, lag).strength / lowScale);

            return contrast * tempoPrior (bpm);
        };

        auto refinedLag = 0.0;
        auto bestScore = 0.0;

        for (const auto lag : toTest)
        {
            const auto candidate = refine (lag);

            if (const auto score = scoreFor (candidate); score > bestScore)
            {
                bestScore = score;
                refinedLag = candidate;
            }
        }

        if (refinedLag <= 0.0 || bestScore <= 0.0)
            return {};

        // Confidence is how far clear the winner finished of the nearest tempo
        // that is not simply a rounding of it. A track with two equally good
        // readings should say so rather than claiming certainty.
        const auto winnerBpm = 60.0 * framesPerSecond / refinedLag;
        auto runnerUp = 0.0;

        for (const auto lag : toTest)
        {
            const auto candidate = refine (lag);
            const auto bpm = 60.0 * framesPerSecond / candidate;

            if (std::abs (bpm - winnerBpm) / winnerBpm < 0.03)
                continue;

            runnerUp = juce::jmax (runnerUp, scoreFor (candidate));
        }

        TempoEstimate estimate;
        estimate.periodFrames = refinedLag;
        estimate.bpm = winnerBpm;
        estimate.confidence = runnerUp > 0.0
            ? (float) juce::jlimit (0.0, 1.0, (1.0 - runnerUp / bestScore) * 2.5)
            : 1.0f;

        return estimate;
    }

    /** Slides a pulse train across the envelope to find where the beats sit. */
    double findFirstBeat (const std::vector<float>& envelope, double periodFrames, double framesPerSecond)
    {
        if (periodFrames < 2.0 || envelope.empty())
            return 0.0;

        const auto bestOffset = fitPulseTrain (envelope, periodFrames).offset;

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

namespace
{
    /** The waveforms and sample rate, which every analysis starts with.
        Returns false when there is nothing to analyse. */
    bool buildWaveforms (TrackAnalysis& analysis,
                         const juce::AudioBuffer<float>& audio,
                         double sampleRate,
                         const TrackAnalyser::Options& options)
    {
        analysis.sampleRate = sampleRate;

        const auto numSamples = audio.getNumSamples();

        if (numSamples <= 0 || sampleRate <= 0.0)
            return false;

        const auto overviewBucket = juce::jmax (1, numSamples / juce::jmax (1, options.overviewBuckets));
        const auto detailBucket = juce::jmax (1, static_cast<int> (sampleRate * options.detailBucketMs / 1000.0));

        analysis.overview = TrackAnalyser::buildPeaks (audio, overviewBucket);
        analysis.detail = TrackAnalyser::buildPeaks (audio, detailBucket);
        return true;
    }
}

std::shared_ptr<const TrackAnalysis> TrackAnalyser::withKnownTempo (const juce::AudioBuffer<float>& audio,
                                                                    double sampleRate,
                                                                    double bpm,
                                                                    double firstBeatSeconds,
                                                                    float confidence,
                                                                    Options options)
{
    auto analysis = std::make_shared<TrackAnalysis>();

    if (! buildWaveforms (*analysis, audio, sampleRate, options))
        return analysis;

    if (bpm > 0.0)
    {
        analysis->bpm = bpm;
        analysis->firstBeatSeconds = juce::jmax (0.0, firstBeatSeconds);
        analysis->confidence = juce::jlimit (0.0f, 1.0f, confidence);
    }

    return analysis;
}

std::shared_ptr<const TrackAnalysis> TrackAnalyser::analyse (const juce::AudioBuffer<float>& audio,
                                                             double sampleRate,
                                                             Options options)
{
    auto analysis = std::make_shared<TrackAnalysis>();

    if (! buildWaveforms (*analysis, audio, sampleRate, options))
        return analysis;

    const auto envelopes = onsetEnvelopes (toMono (audio), sampleRate);
    const auto& envelope = envelopes.full;

    if (envelope.empty())
        return analysis;

    const auto framesPerSecond = sampleRate / hopSize;
    const auto tempo = estimateTempo (envelope, envelopes.low, framesPerSecond,
                                      options.minimumBpm, options.maximumBpm);

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
