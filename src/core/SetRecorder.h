/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace opendj
{

/** What the recorder writes, and where.

    Kept beside the recorder rather than in `SessionState`, the same way
    `BroadcastSettings` lives beside the Icecast connection: the thing that
    consumes a setting is the thing that should define it, and this way the
    core has no idea the application has a settings file.
*/
enum class RecordingFormat
{
    wav,    ///< 24-bit. The default: a recording is a master to work from later.
    flac,   ///< 24-bit, lossless, about half the size.
    mp3     ///< Lossy, and about a tenth. For sending somebody the set.
};

struct RecordingSettings
{
    RecordingFormat format = RecordingFormat::wav;

    /** A constant bitrate in kilobits, or 0 for LAME's V0, which is variable
        and averages around 245. Ignored unless the format is `mp3`. */
    int mp3Bitrate = 320;

    /** Empty means `SetRecorder::defaultFolder()`, which is what it stays
        unless somebody goes looking for the setting. */
    juce::String folder;
};

/** Records the master output to a file while you play.

    The audio thread only ever hands blocks to JUCE's ThreadedWriter, which
    copies them into a FIFO and returns. A background thread does the encoding
    and the disk write, so nothing here blocks the audio callback, and a slow
    disk drops samples from the recording rather than from what the room hears.

    Alongside the audio it keeps a tracklist: what was playing and when it
    started, measured against the recording rather than the clock. A two hour
    set is unusable without one.
*/
class SetRecorder
{
public:
    SetRecorder();
    ~SetRecorder();

    //==========================================================================
    // Message thread
    //==========================================================================

    /** Starts recording to a new file, named for the date and time and given
        the extension the chosen format asks for. Returns the file, or an
        invalid file with `error` filled in. */
    juce::File start (const RecordingSettings& settings, double sampleRate, juce::String& error);

    /** Stops, finishes the file and writes the tracklist beside it. Returns the
        audio file that was written, or an invalid file if nothing was. */
    juce::File stop();

    bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }

    /** How long the recording is, in seconds. This is the length of the file,
        not the time since start: if the disk could not keep up, they differ. */
    double getRecordedSeconds() const noexcept;

    /** How much audio the FIFO between the audio thread and the disk holds.

        Roughly two seconds at 48 kHz. Public because it is the one number that
        decides whether a block can be taken at all: anything larger than this
        can never fit, whatever the disk is doing, which is what lets the
        dropped-sample accounting be tested without racing a real writer. */
    static constexpr int fifoSamples = 96000;

    /** Samples the disk could not keep up with, which are missing from the file.
        Dropping them is the right trade against stalling the audio thread, but
        it is not something to do quietly: a set with a hole in it should say so
        while there is still time to do something about it. */
    juce::int64 getDroppedSamples() const noexcept { return droppedSamples.load (std::memory_order_relaxed); }
    bool hadDropouts() const noexcept { return getDroppedSamples() > 0; }

    /** The file being written, or an invalid file when stopped. */
    juce::File getFile() const;

    /** Notes that a track started playing, at the current point in the
        recording. Ignored when not recording, so callers need not check. */
    void noteTrack (const juce::String& title);

    struct Entry
    {
        double seconds = 0.0;
        juce::String title;

        /** "0:00:00 Artist - Title", the way a tracklist is written. */
        juce::String toString() const;
    };

    std::vector<Entry> getTracklist() const;

    /** Where a recording goes when nowhere else is chosen: the user's music
        folder, under OpenDJ. */
    static juce::File defaultFolder();

    /** ".wav", ".flac" or ".mp3". */
    static juce::String extensionFor (RecordingFormat format);

    /** "WAV", "FLAC" or "MP3", for the one error message and the dialog. */
    static juce::String nameFor (RecordingFormat format);

    //==========================================================================
    // Audio thread
    //==========================================================================

    /** Hands one block of master audio to the writer. Does nothing when not
        recording, and never blocks. */
    void write (const juce::AudioBuffer<float>& master, int numSamples);

private:
    juce::File writeTracklist (const juce::File& audioFile) const;

    /** The one place a format turns into a writer. Null when the format could
        not be started at this sample rate, and the stream is thrown away with
        it. */
    static std::unique_ptr<juce::AudioFormatWriter> makeWriter (
        const RecordingSettings& settings, std::unique_ptr<juce::FileOutputStream> stream,
        double sampleRate);

    // The writer is created on the message thread and used from the audio
    // thread, so the pointer the audio thread reads is swapped under a lock the
    // audio thread never takes: it only ever reads the atomic.
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };

    juce::TimeSliceThread writerThread { "OpenDJ recording" };

    std::atomic<bool> recording { false };
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<juce::int64> droppedSamples { 0 };
    std::atomic<double> currentSampleRate { 44100.0 };

    mutable std::mutex detailMutex;
    juce::File currentFile;
    std::vector<Entry> tracklist;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetRecorder)
};

} // namespace opendj
