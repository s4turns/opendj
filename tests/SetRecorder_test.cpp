/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Mp3Writer.h"
#include "core/SetRecorder.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using opendj::Mp3Writer;
using opendj::RecordingFormat;
using opendj::RecordingSettings;
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

    /** WAV unless asked otherwise, which is also the recorder's own default. */
    RecordingSettings into (const Folder& folder,
                            RecordingFormat format = RecordingFormat::wav,
                            int mp3Bitrate = 320)
    {
        RecordingSettings settings;
        settings.format = format;
        settings.mp3Bitrate = mp3Bitrate;
        settings.folder = folder.dir.getFullPathName();
        return settings;
    }

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

TEST_CASE ("a recording produces a playable file of the right length", "[recorder][format]")
{
    const auto format = GENERATE (RecordingFormat::wav, RecordingFormat::flac,
                                  RecordingFormat::mp3);

    INFO ("format: " << SetRecorder::nameFor (format));

    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (into (folder, format), sampleRate, error);

    INFO ("error: " << error);
    REQUIRE (error.isEmpty());
    REQUIRE (recorder.isRecording());
    REQUIRE (file.getFileExtension() == SetRecorder::extensionFor (format));

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

    if (format == RecordingFormat::mp3)
    {
        // Not sample-exact, and cannot be. LAME pads the first frame with its
        // own encoder delay and the last one out to a frame boundary, and JUCE
        // over-reports an MP3's length on top of that (see ROADMAP.md). A
        // quarter of a second either way catches a file that is the wrong
        // length for a real reason while leaving the format its own padding.
        REQUIRE_THAT ((double) reader->lengthInSamples / sampleRate,
                      WithinAbs ((double) kept / sampleRate, 0.25));
    }
    else
    {
        REQUIRE (reader->lengthInSamples == kept);
    }

    juce::AudioBuffer<float> back (2, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&back, 0, (int) reader->lengthInSamples, 0, true, true));
    REQUIRE (back.getMagnitude (0, 0, back.getNumSamples()) > 0.2f);
}

TEST_CASE ("an MP3 recording is written at a rate MP3 has", "[recorder][format]")
{
    // A DJ interface at 96 kHz is ordinary and MP3 has no such rate, so LAME is
    // asked to resample rather than refusing the recording. This is the one
    // path that would otherwise be quietly broken on exactly the hardware most
    // likely to be in front of it.
    constexpr double deviceRate = 96000.0;

    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (into (folder, RecordingFormat::mp3), deviceRate, error);

    INFO ("error: " << error);
    REQUIRE (error.isEmpty());

    writeBlocks (recorder, 100);
    recorder.stop();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    REQUIRE_THAT (reader->sampleRate, WithinAbs (48000.0, 0.1));
    REQUIRE (reader->lengthInSamples > 0);
}

TEST_CASE ("a rate MP3 cannot carry becomes the nearest one it can", "[recorder][format]")
{
    // By ratio rather than by difference: 88.2 is a doubled 44.1 and halving it
    // is a clean decimation, where 48 is merely the closer of the two in Hz.
    REQUIRE (Mp3Writer::nearestSupportedRate (44100.0) == 44100);
    REQUIRE (Mp3Writer::nearestSupportedRate (48000.0) == 48000);
    REQUIRE (Mp3Writer::nearestSupportedRate (88200.0) == 44100);
    REQUIRE (Mp3Writer::nearestSupportedRate (96000.0) == 48000);
    REQUIRE (Mp3Writer::nearestSupportedRate (176400.0) == 44100);
    REQUIRE (Mp3Writer::nearestSupportedRate (192000.0) == 48000);
    REQUIRE (Mp3Writer::nearestSupportedRate (32000.0) == 32000);

    // Nothing divides it, so the closest one there is.
    REQUIRE (Mp3Writer::nearestSupportedRate (50000.0) == 48000);
}

TEST_CASE ("a variable bitrate MP3 records too", "[recorder][format]")
{
    // Zero means V0, which is a different code path through LAME from a
    // constant bitrate and the one that needs the Xing header rewritten on the
    // way out.
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (into (folder, RecordingFormat::mp3, 0), sampleRate, error);

    INFO ("error: " << error);
    REQUIRE (error.isEmpty());

    writeBlocks (recorder, 100);
    recorder.stop();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->lengthInSamples > 0);
}

TEST_CASE ("the tracklist sits beside the audio whatever the format", "[recorder][format]")
{
    const auto format = GENERATE (RecordingFormat::wav, RecordingFormat::flac,
                                  RecordingFormat::mp3);

    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (into (folder, format), sampleRate, error);
    REQUIRE (error.isEmpty());

    recorder.noteTrack ("Boys Noize - Oh!");
    writeBlocks (recorder, 10);
    recorder.stop();

    const auto listFile = file.withFileExtension (".txt");
    INFO (listFile.getFullPathName());
    REQUIRE (listFile.existsAsFile());
    REQUIRE (listFile.loadFileAsString().contains ("Boys Noize - Oh!"));
}

TEST_CASE ("samples the disk could not take are counted, not lost quietly", "[recorder]")
{
    // Dropping is the right trade against stalling the audio thread, but a set
    // with a hole in it has to say so rather than look complete.
    //
    // The refusal is provoked with a block larger than the FIFO, which can
    // never fit whatever the disk is doing. Racing a real background writer
    // with a burst of ordinary blocks was tried first and is not a test: it
    // passed only when the machine happened to be slower than the loop
    // generating the audio, and failed about one run in fifteen both on the
    // Debian CI job and locally.
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto file = recorder.start (into (folder), sampleRate, error);
    REQUIRE (error.isEmpty());

    // Real audio first, so the hole is a hole in something.
    writeBlocks (recorder, 20);
    const auto droppedBefore = recorder.getDroppedSamples();

    const auto tooBig = SetRecorder::fifoSamples * 2;
    juce::AudioBuffer<float> oversized (2, tooBig);
    oversized.clear();
    recorder.write (oversized, tooBig);

    const auto dropped = recorder.getDroppedSamples();
    INFO ("dropped " << dropped << " samples, " << droppedBefore << " of them before the big block");
    REQUIRE (dropped - droppedBefore == tooBig);
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
    const auto file = recorder.start (into (folder), sampleRate, error);
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
    recorder.start (into (folder), sampleRate, error);

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
    recorder.start (into (folder), sampleRate, error);
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
    REQUIRE (recorder.start (into (folder), sampleRate, error).existsAsFile());

    const auto second = recorder.start (into (folder), sampleRate, error);

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
    const auto file = recorder.start (into (folder), 0.0, error);

    REQUIRE (file == juce::File());
    REQUIRE (error.contains ("no audio device"));
}

TEST_CASE ("two recordings in a row do not overwrite each other", "[recorder]")
{
    Folder folder;
    SetRecorder recorder;

    juce::String error;
    const auto first = recorder.start (into (folder), sampleRate, error);
    writeBlocks (recorder, 5);
    recorder.stop();

    const auto second = recorder.start (into (folder), sampleRate, error);
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
