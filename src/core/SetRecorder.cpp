/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/SetRecorder.h"

namespace opendj
{

namespace
{
    // 24 bit, because a recording is a master to work from later rather than a
    // delivery format, and the extra eight bits cost only disk.
    constexpr int bitsPerSample = 24;

    juce::String twoDigits (int value)
    {
        return juce::String (value).paddedLeft ('0', 2);
    }
}

juce::String SetRecorder::Entry::toString() const
{
    const auto total = static_cast<int> (seconds);

    return juce::String (total / 3600) + ":" + twoDigits ((total / 60) % 60)
         + ":" + twoDigits (total % 60) + "  " + title;
}

SetRecorder::SetRecorder()
{
    writerThread.startThread (juce::Thread::Priority::normal);
}

SetRecorder::~SetRecorder()
{
    stop();
    writerThread.stopThread (2000);
}

juce::File SetRecorder::defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
               .getChildFile ("OpenDJ");
}

juce::File SetRecorder::start (const juce::File& folder, double sampleRate, juce::String& error)
{
    error.clear();

    if (isRecording())
    {
        error = "Already recording.";
        return {};
    }

    if (sampleRate <= 0.0)
    {
        error = "There is no audio device open to record from.";
        return {};
    }

    if (const auto created = folder.createDirectory(); created.failed())
    {
        error = "Could not make " + folder.getFullPathName() + ": " + created.getErrorMessage();
        return {};
    }

    // Named for when it was played, which is how anyone looks for a set later.
    const auto now = juce::Time::getCurrentTime();
    const auto name = "OpenDJ " + juce::String (now.getYear())
                    + "-" + twoDigits (now.getMonth() + 1)
                    + "-" + twoDigits (now.getDayOfMonth())
                    + " " + twoDigits (now.getHours())
                    + "." + twoDigits (now.getMinutes())
                    + "." + twoDigits (now.getSeconds());

    auto file = folder.getChildFile (name + ".wav");
    file = file.getNonexistentSibling();

    auto stream = std::make_unique<juce::FileOutputStream> (file);

    if (! stream->openedOk())
    {
        error = "Could not open " + file.getFullPathName() + " for writing.";
        return {};
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> formatWriter (
        wav.createWriterFor (stream.get(), sampleRate, 2, bitsPerSample, {}, 0));

    if (formatWriter == nullptr)
    {
        error = "Could not start a WAV file at " + juce::String (sampleRate, 0) + " Hz.";
        return {};
    }

    // The writer owns the stream from here.
    stream.release();

    writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
        formatWriter.release(), writerThread, SetRecorder::fifoSamples);

    {
        std::lock_guard<std::mutex> lock (detailMutex);
        currentFile = file;
        tracklist.clear();
    }

    samplesWritten.store (0, std::memory_order_relaxed);
    droppedSamples.store (0, std::memory_order_relaxed);
    currentSampleRate.store (sampleRate, std::memory_order_relaxed);

    // Published last, so the audio thread never sees a writer that is not ready.
    activeWriter.store (writer.get(), std::memory_order_release);
    recording.store (true, std::memory_order_release);

    return file;
}

juce::File SetRecorder::stop()
{
    if (! recording.exchange (false, std::memory_order_acq_rel))
        return {};

    // Stop the audio thread using it before it is destroyed. The callback reads
    // this pointer at the top of every block, so once it is null and a block has
    // passed nothing is inside the writer.
    activeWriter.store (nullptr, std::memory_order_release);

    // Destroying the ThreadedWriter flushes what is still in the FIFO and
    // finishes the file's header.
    writer.reset();

    juce::File file;

    {
        std::lock_guard<std::mutex> lock (detailMutex);
        file = currentFile;
        currentFile = juce::File();
    }

    if (file.existsAsFile())
        writeTracklist (file);

    return file;
}

double SetRecorder::getRecordedSeconds() const noexcept
{
    const auto rate = currentSampleRate.load (std::memory_order_relaxed);

    return rate > 0.0
        ? static_cast<double> (samplesWritten.load (std::memory_order_relaxed)) / rate
        : 0.0;
}

juce::File SetRecorder::getFile() const
{
    std::lock_guard<std::mutex> lock (detailMutex);
    return currentFile;
}

void SetRecorder::noteTrack (const juce::String& title)
{
    if (! isRecording() || title.isEmpty())
        return;

    std::lock_guard<std::mutex> lock (detailMutex);

    // The same track twice running is a reload, not a new entry in the list.
    if (! tracklist.empty() && tracklist.back().title == title)
        return;

    tracklist.push_back ({ getRecordedSeconds(), title });
}

std::vector<SetRecorder::Entry> SetRecorder::getTracklist() const
{
    std::lock_guard<std::mutex> lock (detailMutex);
    return tracklist;
}

juce::File SetRecorder::writeTracklist (const juce::File& audioFile) const
{
    const auto listFile = audioFile.withFileExtension (".txt");

    juce::String text;
    text << audioFile.getFileNameWithoutExtension() << juce::newLine
         << juce::String (getRecordedSeconds(), 1) << " seconds" << juce::newLine
         << juce::newLine;

    std::vector<Entry> entries;

    {
        std::lock_guard<std::mutex> lock (detailMutex);
        entries = tracklist;
    }

    if (entries.empty())
        text << "No tracks were loaded while this was recording." << juce::newLine;
    else
        for (const auto& entry : entries)
            text << entry.toString() << juce::newLine;

    // Said plainly and in the file itself, because by the time anyone listens
    // back it is far too late to ask whether the disk kept up.
    if (const auto dropped = getDroppedSamples(); dropped > 0)
    {
        const auto rate = currentSampleRate.load (std::memory_order_relaxed);

        text << juce::newLine
             << "WARNING: " << juce::String (dropped) << " samples ("
             << juce::String (rate > 0.0 ? dropped / rate : 0.0, 2)
             << " seconds) were dropped because the disk could not keep up."
             << " The recording has gaps." << juce::newLine;
    }

    listFile.replaceWithText (text);
    return listFile;
}

//==============================================================================
// Audio thread
//==============================================================================

void SetRecorder::write (const juce::AudioBuffer<float>& master, int numSamples)
{
    auto* target = activeWriter.load (std::memory_order_acquire);

    if (target == nullptr || numSamples <= 0)
        return;

    // ThreadedWriter::write copies into a FIFO and returns; the encoding and the
    // disk write happen on the background thread. It returns false when the FIFO
    // is full, which costs the recording those samples and costs the output
    // nothing. That is the right way round.
    if (target->write (master.getArrayOfReadPointers(), numSamples))
        samplesWritten.fetch_add (numSamples, std::memory_order_relaxed);
    else
        droppedSamples.fetch_add (numSamples, std::memory_order_relaxed);
}

} // namespace opendj
