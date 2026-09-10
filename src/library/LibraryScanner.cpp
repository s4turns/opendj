/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "library/LibraryScanner.h"

#include "analysis/TrackAnalyser.h"
#include "analysis/TrackDecoder.h"
#include "library/TagReader.h"

namespace opendj
{

LibraryScanner::LibraryScanner (Library& libraryToUse)
    : juce::Thread ("Library scanner"),
      library (libraryToUse)
{
    formatManager.registerBasicFormats();
}

LibraryScanner::~LibraryScanner()
{
    stop();
}

void LibraryScanner::start()
{
    if (isThreadRunning())
    {
        runAgain.store (true, std::memory_order_relaxed);
        return;
    }

    // Analysis is a background job in the truest sense: the decks and the
    // interface come first, whatever the size of the collection.
    startThread (juce::Thread::Priority::low);
}

void LibraryScanner::stop()
{
    runAgain.store (false, std::memory_order_relaxed);
    stopThread (5000);
}

LibraryScanner::Progress LibraryScanner::getProgress() const
{
    std::lock_guard<std::mutex> lock (progressMutex);
    return progress;
}

void LibraryScanner::report()
{
    if (onProgress != nullptr)
        onProgress();
}

void LibraryScanner::run()
{
    do
    {
        runAgain.store (false, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock (progressMutex);
            progress = {};
            progress.running = true;
            progress.scanning = true;
        }

        report();
        scanFolders();

        if (! threadShouldExit())
            pruneMissing();

        {
            std::lock_guard<std::mutex> lock (progressMutex);
            progress.scanning = false;
        }

        report();

        if (! threadShouldExit())
            analysePending();
    }
    while (runAgain.load (std::memory_order_relaxed) && ! threadShouldExit());

    {
        std::lock_guard<std::mutex> lock (progressMutex);
        progress.running = false;
        progress.currentFile.clear();
    }

    report();
}

void LibraryScanner::scanFolders()
{
    for (const auto& folder : library.getFolders())
    {
        if (threadShouldExit())
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (threadShouldExit())
                return;

            const auto file = entry.getFile();

            if (formatManager.findFormatForFileExtension (file.getFileExtension()) == nullptr)
                continue;

            {
                std::lock_guard<std::mutex> lock (progressMutex);
                ++progress.filesFound;
                progress.currentFile = file.getFileName();
            }

            // A file the library has already seen, unchanged, costs one query.
            if (const auto existing = library.findTrack (file);
                existing.has_value()
                && existing->fileSize == file.getSize()
                && existing->fileModified == file.getLastModificationTime().toMilliseconds())
            {
                std::lock_guard<std::mutex> lock (progressMutex);
                ++progress.filesScanned;
                continue;
            }

            const auto tags = TagReader::read (formatManager, file);

            TrackRecord record;
            record.file = file;
            record.title = tags.title;
            record.artist = tags.artist;
            record.album = tags.album;
            record.genre = tags.genre;
            record.key = tags.key;
            record.durationSeconds = tags.durationSeconds;
            record.fileSize = file.getSize();
            record.fileModified = file.getLastModificationTime().toMilliseconds();
            library.upsertTrack (record);

            {
                std::lock_guard<std::mutex> lock (progressMutex);
                ++progress.filesScanned;
            }

            if (progress.filesScanned % 50 == 0)
                report();
        }
    }
}

void LibraryScanner::pruneMissing()
{
    // Only files that were inside a watched folder are dropped; a track loaded
    // from elsewhere and remembered stays remembered.
    for (const auto& folder : library.getFolders())
    {
        LibraryQuery everything;
        everything.folder = folder;
        everything.limit = 1000000;

        for (const auto& record : library.query (everything))
        {
            if (threadShouldExit())
                return;

            if (! record.file.existsAsFile())
                library.removeTrack (record.file);
        }
    }
}

void LibraryScanner::analysePending()
{
    {
        std::lock_guard<std::mutex> lock (progressMutex);
        progress.tracksToAnalyse = library.countTracks() - library.countAnalysed();
    }

    report();

    while (! threadShouldExit())
    {
        const auto pending = library.tracksNeedingAnalysis (1);

        if (pending.empty())
            break;

        const auto file = pending.front();

        {
            std::lock_guard<std::mutex> lock (progressMutex);
            progress.currentFile = file.getFileName();
        }

        report();

        const auto decoded = TrackDecoder::decode (formatManager, file);

        if (decoded == nullptr)
        {
            library.markUnreadable (file);
        }
        else
        {
            const auto analysis = TrackAnalyser::analyse (decoded->audio, decoded->sampleRate);
            library.store (file, *analysis, decoded->lengthSeconds());
        }

        {
            std::lock_guard<std::mutex> lock (progressMutex);
            ++progress.tracksAnalysed;
        }
    }
}

} // namespace opendj
