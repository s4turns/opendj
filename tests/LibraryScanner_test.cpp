/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "library/LibraryScanner.h"

using Catch::Matchers::WithinAbs;

namespace
{
    /** Twenty seconds of clicks at a given tempo. */
    void writeClicks (const juce::File& file, double bpm)
    {
        constexpr double sampleRate = 44100.0;
        const auto numSamples = (int) (20.0 * sampleRate);
        const auto samplesPerBeat = sampleRate * 60.0 / bpm;
        juce::AudioBuffer<float> audio (2, numSamples);
        audio.clear();

        for (double beat = 0.0; beat < numSamples; beat += samplesPerBeat)
            for (int i = 0; i < 400 && (int) beat + i < numSamples; ++i)
            {
                const auto value = 0.9f * std::exp (-(float) i / 80.0f) * ((i / 10) % 2 == 0 ? 1.0f : -1.0f);

                for (int ch = 0; ch < 2; ++ch)
                    audio.setSample (ch, (int) beat + i, value);
            }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        REQUIRE (stream != nullptr);

        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 2, 16, {}, 0));
        REQUIRE (writer != nullptr);
        stream.release();   // the writer owns it now
        writer->writeFromAudioSampleBuffer (audio, 0, numSamples);
    }
}

TEST_CASE ("the scanner analyses every track once, on several workers", "[library]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("opendj-scanner-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    REQUIRE (folder.createDirectory());

    const std::vector<double> tempos { 110.0, 116.0, 122.0, 128.0, 134.0, 140.0 };

    for (const auto bpm : tempos)
        writeClicks (folder.getChildFile (juce::String (bpm, 0) + ".wav"), bpm);

    {
        opendj::Library library;
        REQUIRE (library.open (folder.getChildFile ("library.sqlite")).wasOk());
        REQUIRE (library.addFolder (folder));

        opendj::LibraryScanner scanner (library);
        REQUIRE (opendj::LibraryScanner::analysisWorkerCount() >= 1);
        scanner.start();

        for (int waited = 0; scanner.isRunning() && waited < 120000; waited += 50)
            juce::Thread::sleep (50);

        REQUIRE_FALSE (scanner.isRunning());

        const auto progress = scanner.getProgress();
        REQUIRE (progress.tracksAnalysed == (int) tempos.size());
        REQUIRE (library.countAnalysed() == (int) tempos.size());

        // Each result landed on its own file, not a neighbour's.
        for (const auto bpm : tempos)
        {
            const auto track = library.findTrack (folder.getChildFile (juce::String (bpm, 0) + ".wav"));
            REQUIRE (track.has_value());
            REQUIRE (track->analysed);
            REQUIRE_THAT (track->bpm, WithinAbs (bpm, 1.0));
        }

        library.close();
    }

    folder.deleteRecursively();
}
