/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "analysis/TrackAnalyser.h"

#include <cmath>

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr double sampleRate = 44100.0;

    /** A four-to-the-floor pattern at a known tempo: a decaying low thump on
        every beat and a shorter click on the offbeats, which is enough spectral
        movement for the onset detector to work with. */
    juce::AudioBuffer<float> makeBeatPattern (double bpm, double seconds, double startOffsetSeconds = 0.0)
    {
        const auto numSamples = static_cast<int> (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);
        buffer.clear();

        const auto samplesPerBeat = 60.0 / bpm * sampleRate;
        const auto firstBeat = startOffsetSeconds * sampleRate;

        const auto addHit = [&buffer, numSamples] (double startSample, double frequency,
                                                   double decaySamples, float amplitude)
        {
            const auto start = static_cast<int> (startSample);

            for (int i = 0; i < static_cast<int> (decaySamples * 4.0); ++i)
            {
                const auto index = start + i;

                if (index < 0 || index >= numSamples)
                    break;

                const auto envelope = std::exp (-i / decaySamples);
                const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
                const auto value = static_cast<float> (std::sin (phase) * envelope) * amplitude;

                buffer.addSample (0, index, value);
                buffer.addSample (1, index, value);
            }
        };

        for (double beat = 0.0; ; beat += 1.0)
        {
            const auto position = firstBeat + beat * samplesPerBeat;

            if (position >= numSamples)
                break;

            addHit (position, 55.0, sampleRate * 0.06, 0.8f);                          // kick
            addHit (position + samplesPerBeat * 0.5, 6000.0, sampleRate * 0.01, 0.3f); // hat
        }

        return buffer;
    }
}

TEST_CASE ("waveform peaks summarise the signal", "[analysis][waveform]")
{
    juce::AudioBuffer<float> buffer (2, 10000);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 10000; ++i)
            buffer.setSample (ch, i, i < 5000 ? 0.5f : -0.25f);

    const auto peaks = opendj::TrackAnalyser::buildPeaks (buffer, 1000);

    REQUIRE (peaks.buckets.size() == 10);
    REQUIRE_THAT (peaks.buckets.front().maximum, WithinAbs (0.5f, 0.001f));
    REQUIRE_THAT (peaks.buckets.front().minimum, WithinAbs (0.0f, 0.001f));
    REQUIRE_THAT (peaks.buckets.back().minimum, WithinAbs (-0.25f, 0.001f));
    REQUIRE_THAT (peaks.buckets.back().energy, WithinAbs (0.25f, 0.001f));
}

TEST_CASE ("peaks cover the whole track even when it does not divide evenly", "[analysis][waveform]")
{
    juce::AudioBuffer<float> buffer (2, 2500);
    buffer.clear();

    const auto peaks = opendj::TrackAnalyser::buildPeaks (buffer, 1000);

    REQUIRE (peaks.buckets.size() == 3);
}

namespace
{
    /** A steady tone, loud enough that every band reading is well clear of the
        crossover's own skirts. */
    juce::AudioBuffer<float> makeTone (double frequency, double seconds)
    {
        const auto numSamples = static_cast<int> (seconds * sampleRate);
        juce::AudioBuffer<float> buffer (2, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * frequency * i / sampleRate;
            const auto value = static_cast<float> (std::sin (phase)) * 0.8f;

            buffer.setSample (0, i, value);
            buffer.setSample (1, i, value);
        }

        return buffer;
    }

    opendj::WaveformPeaks peaksWithBands (const juce::AudioBuffer<float>& audio, int samplesPerBucket)
    {
        auto peaks = opendj::TrackAnalyser::buildPeaks (audio, samplesPerBucket);
        opendj::TrackAnalyser::addBandEnergies (audio, sampleRate, { &peaks });
        return peaks;
    }

    /** The middle bucket, so the filters' settling at the very start of the
        track is not what is being measured. */
    const opendj::WaveformPeaks::Bucket& middleOf (const opendj::WaveformPeaks& peaks)
    {
        return peaks.buckets[peaks.buckets.size() / 2];
    }
}

TEST_CASE ("a bass tone colours the low band", "[analysis][waveform]")
{
    const auto peaks = peaksWithBands (makeTone (60.0, 2.0), 4410);
    const auto& bucket = middleOf (peaks);

    REQUIRE (bucket.low > bucket.mid);
    REQUIRE (bucket.low > bucket.high);
    REQUIRE (bucket.mid < bucket.low * 0.1f);
    REQUIRE (bucket.high < bucket.low * 0.1f);
}

TEST_CASE ("a midrange tone colours the mid band", "[analysis][waveform]")
{
    const auto peaks = peaksWithBands (makeTone (1000.0, 2.0), 4410);
    const auto& bucket = middleOf (peaks);

    REQUIRE (bucket.mid > bucket.low);
    REQUIRE (bucket.mid > bucket.high);
}

TEST_CASE ("a treble tone colours the high band", "[analysis][waveform]")
{
    const auto peaks = peaksWithBands (makeTone (10000.0, 2.0), 4410);
    const auto& bucket = middleOf (peaks);

    REQUIRE (bucket.high > bucket.low);
    REQUIRE (bucket.high > bucket.mid);
}

TEST_CASE ("dominance passes from low to mid to high across the crossovers", "[analysis][waveform]")
{
    // The behaviour worth pinning down, rather than any exact figure: a
    // Linkwitz-Riley crossover's halves add in amplitude, not in power, so the
    // three bands of a bucket have no reason to sum to its energy.
    const auto dominantBand = [] (double frequency)
    {
        const auto& bucket = middleOf (peaksWithBands (makeTone (frequency, 2.0), 4410));

        if (bucket.low >= bucket.mid && bucket.low >= bucket.high)
            return 0;

        return bucket.mid >= bucket.high ? 1 : 2;
    };

    REQUIRE (dominantBand (80.0) == 0);
    REQUIRE (dominantBand (800.0) == 1);
    REQUIRE (dominantBand (8000.0) == 2);
}

TEST_CASE ("silence leaves every band at zero", "[analysis][waveform]")
{
    juce::AudioBuffer<float> buffer (2, 44100);
    buffer.clear();

    const auto peaks = peaksWithBands (buffer, 4410);

    REQUIRE_THAT (middleOf (peaks).low, WithinAbs (0.0f, 0.0001f));
    REQUIRE_THAT (middleOf (peaks).mid, WithinAbs (0.0f, 0.0001f));
    REQUIRE_THAT (middleOf (peaks).high, WithinAbs (0.0f, 0.0001f));
}

TEST_CASE ("tempo detection recovers a known BPM", "[analysis][tempo]")
{
    // House, drum and bass, and the slow end of hip hop, so the whole search
    // range gets exercised rather than one comfortable tempo.
    for (const double bpm : { 90.0, 120.0, 128.0, 174.0 })
    {
        const auto buffer = makeBeatPattern (bpm, 20.0);
        const auto analysis = opendj::TrackAnalyser::analyse (buffer, sampleRate);

        INFO ("expected " << bpm << " BPM, measured " << analysis->bpm);
        REQUIRE (analysis->hasTempo());
        REQUIRE_THAT (analysis->bpm, WithinAbs (bpm, 1.0));
    }
}

TEST_CASE ("the beat grid lands on the first beat", "[analysis][tempo]")
{
    const double offset = 0.37;
    const auto buffer = makeBeatPattern (128.0, 20.0, offset);
    const auto analysis = opendj::TrackAnalyser::analyse (buffer, sampleRate);

    REQUIRE (analysis->hasTempo());

    // The anchor may be any beat, not necessarily the first, so check that the
    // grid it implies actually passes through the beat we placed.
    const auto period = analysis->secondsPerBeat();
    auto distance = std::fmod (std::abs (analysis->firstBeatSeconds - offset), period);
    distance = juce::jmin (distance, period - distance);

    INFO ("grid anchor " << analysis->firstBeatSeconds << ", beat at " << offset);
    REQUIRE (distance < 0.03);   // within 30 ms, close enough to look aligned
}

TEST_CASE ("the beat grid snaps positions to beats", "[analysis][tempo]")
{
    const auto buffer = makeBeatPattern (120.0, 20.0);
    const auto analysis = opendj::TrackAnalyser::analyse (buffer, sampleRate);

    REQUIRE (analysis->hasTempo());

    const auto period = analysis->secondsPerBeat();
    const auto beat = analysis->firstBeatSeconds + 8.0 * period;

    // A position just past a beat should snap back to it, not on to the next.
    REQUIRE_THAT (analysis->nearestBeatSeconds (beat + period * 0.1), WithinAbs (beat, 0.001));
    REQUIRE_THAT (analysis->nearestBeatSeconds (beat - period * 0.1), WithinAbs (beat, 0.001));
}

TEST_CASE ("silence yields no tempo rather than a fabricated one", "[analysis][tempo]")
{
    juce::AudioBuffer<float> buffer (2, static_cast<int> (sampleRate * 5.0));
    buffer.clear();

    const auto analysis = opendj::TrackAnalyser::analyse (buffer, sampleRate);

    REQUIRE_FALSE (analysis->hasTempo());
}

TEST_CASE ("analysis of an empty buffer is harmless", "[analysis]")
{
    juce::AudioBuffer<float> buffer (2, 0);
    const auto analysis = opendj::TrackAnalyser::analyse (buffer, sampleRate);

    REQUIRE (analysis != nullptr);
    REQUIRE (analysis->overview.isEmpty());
    REQUIRE_FALSE (analysis->hasTempo());
}
