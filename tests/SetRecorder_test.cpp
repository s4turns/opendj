/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/SetRecorder.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using opendj::SetRecorder;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    /** A folder that cleans itself up, since the recorder writes real files. */
    struct Folder
    {
        Folder()
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("opendj-recorder-test-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
            dir.createDirectory();
        }

        ~Folder() { dir.deleteRecursively(); }

        juce::File dir;
    };

    void writeBlocks (SetRecorder& recorder, int blocks, float amplitude = 0.25f)
    {
        juce::AudioBuffer<float> master (2, blockSize);
        double phase = 0.0;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto value = static_cast<float> (std::sin (phase)) * amplitude;
                phase += juce::MathConstants<double>::twoPi * 440.0 / sampleRate;

                master.setSample (0, i, value);
                master.setSample (1, i, value);
            }

            recorder.write (master, blockSize);
        }
    }
}

TEST_CASE ("a recorder starts idle", "[recorder]")
{
    SetRecorder recorder;

    REQUIRE_FALSE (recorder.isRecording());
    REQUIRE (recorder.getRecordedSeconds() == 0.0);
    REQUIRE (recorder.getFile() == juce::File());
}

TEST_CASE ("writing while stopped does nothing at all", "[recorder]")
{
    // The audio thread calls write() on every block whether or not anyone is
    // recording, so the stopped path has to be safe and silent.
    SetRecorder recorder;
    writeBlocks (recorder, 10);

    REQUIRE_FALSE (recorder.isRecording());
    REQUIRE (recorder.getRecordedSeconds() == 0.0);
}

TEST_CASE ("a recording produces a playable file of the right length", "[recorder]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (folder.dir, sampleRate, error);

    INFO ("error: " << error);
    REQUIRE (error.isEmpty());
    REQUIRE (recorder.isRecording());

    const auto blocks = 200;                       // a little over two seconds
    writeBlocks (recorder, blocks);

    // Not the number of blocks handed over: this test writes two seconds of
    // audio as fast as the CPU allows, so the FIFO fills and the recorder drops
    // what the disk could not take. Whatever it kept has to add up.
    const auto offered = static_cast<juce::int64> (blocks) * blockSize;
    const auto kept = static_cast<juce::int64> (recorder.getRecordedSeconds() * sampleRate + 0.5);

    REQUIRE (kept + recorder.getDroppedSamples() == offered);
    REQUIRE (kept > 0);

    const auto written = recorder.stop();
    REQUIRE_FALSE (recorder.isRecording());
    REQUIRE (written == file);
    REQUIRE (file.existsAsFile());

    // Read it back rather than trusting the byte count: a header written wrong
    // gives a file of the right size that nothing will open.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 2);
    REQUIRE_THAT (reader->sampleRate, WithinAbs (sampleRate, 0.1));
    REQUIRE (reader->lengthInSamples == kept);

    juce::AudioBuffer<float> back (2, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&back, 0, (int) reader->lengthInSamples, 0, true, true));
    REQUIRE (back.getMagnitude (0, 0, back.getNumSamples()) > 0.2f);
}

TEST_CASE ("samples the disk could not take are counted, not lost quietly", "[recorder]")
{
    // Faster than any disk: the FIFO fills and write() starts refusing. Dropping
    // is the right trade against stalling the audio thread, but a set with a
    // hole in it has to say so rather than look complete.
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (folder.dir, sampleRate, error);
    REQUIRE (error.isEmpty());

    writeBlocks (recorder, 2000);                  // twenty-one seconds, instantly

    const auto dropped = recorder.getDroppedSamples();
    INFO ("dropped " << dropped << " samples");
    REQUIRE (dropped > 0);
    REQUIRE (recorder.hadDropouts());

    recorder.stop();

    // And it is written down where it will still be there tomorrow.
    const auto text = file.withFileExtension (".txt").loadFileAsString();
    INFO (text);
    REQUIRE (text.contains ("WARNING"));
    REQUIRE (text.contains ("dropped"));
}

TEST_CASE ("a tracklist is written beside the audio", "[recorder][tracklist]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (folder.dir, sampleRate, error);
    REQUIRE (error.isEmpty());

    recorder.noteTrack ("Boys Noize - Oh!");
    writeBlocks (recorder, 100);
    recorder.noteTrack ("Marcel Dettmann - Seduction");
    writeBlocks (recorder, 100);

    const auto entries = recorder.getTracklist();
    REQUIRE (entries.size() == 2);
    REQUIRE (entries[0].title == "Boys Noize - Oh!");
    REQUIRE (entries[0].seconds == 0.0);

    // The second is timed against the recording, not the wall clock.
    REQUIRE_THAT (entries[1].seconds, WithinAbs (100 * blockSize / sampleRate, 0.01));

    recorder.stop();

    const auto listFile = file.withFileExtension (".txt");
    REQUIRE (listFile.existsAsFile());

    const auto text = listFile.loadFileAsString();
    INFO (text);
    REQUIRE (text.contains ("Boys Noize - Oh!"));
    REQUIRE (text.contains ("Marcel Dettmann - Seduction"));
    REQUIRE (text.contains ("0:00:00"));
    REQUIRE (text.contains ("0:00:01"));
}

TEST_CASE ("the same track twice running is not listed twice", "[recorder][tracklist]")
{
    // Reloading a track onto the other deck to run it back is a reload, not a
    // second play, and a tracklist full of doubled entries is a worse tracklist.
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    recorder.start (folder.dir, sampleRate, error);

    recorder.noteTrack ("One Track");
    recorder.noteTrack ("One Track");
    writeBlocks (recorder, 10);
    recorder.noteTrack ("Another");

    REQUIRE (recorder.getTracklist().size() == 2);
    recorder.stop();
}

TEST_CASE ("tracks noted while stopped are not kept", "[recorder][tracklist]")
{
    Folder folder;
    SetRecorder recorder;

    recorder.noteTrack ("Played before anyone pressed record");
    REQUIRE (recorder.getTracklist().empty());

    juce::String error;
    recorder.start (folder.dir, sampleRate, error);
    REQUIRE (recorder.getTracklist().empty());
    recorder.stop();
}

TEST_CASE ("a tracklist entry reads as a timestamp and a title", "[recorder][tracklist]")
{
    const SetRecorder::Entry entry { 3725.0, "Some Artist - Some Title" };

    REQUIRE (entry.toString() == "1:02:05  Some Artist - Some Title");
}

TEST_CASE ("a second start while recording is refused", "[recorder]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    REQUIRE (recorder.start (folder.dir, sampleRate, error).existsAsFile());

    const auto second = recorder.start (folder.dir, sampleRate, error);

    REQUIRE (second == juce::File());
    REQUIRE (error.contains ("Already recording"));

    // And the first one is still going, rather than having been quietly ended.
    REQUIRE (recorder.isRecording());
    recorder.stop();
}

TEST_CASE ("recording without a sample rate is refused with a reason", "[recorder]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (folder.dir, 0.0, error);

    REQUIRE (file == juce::File());
    REQUIRE (error.contains ("no audio device"));
}

TEST_CASE ("two recordings in a row do not overwrite each other", "[recorder]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto first = recorder.start (folder.dir, sampleRate, error);
    writeBlocks (recorder, 5);
    recorder.stop();

    const auto second = recorder.start (folder.dir, sampleRate, error);
    writeBlocks (recorder, 5);
    recorder.stop();

    REQUIRE (first.existsAsFile());
    REQUIRE (second.existsAsFile());
    REQUIRE (first != second);
}

TEST_CASE ("stopping when nothing is recording is harmless", "[recorder]")
{
    SetRecorder recorder;

    REQUIRE (recorder.stop() == juce::File());
    REQUIRE (recorder.stop() == juce::File());
}
