/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "library/Library.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <mutex>

namespace opendj
{

/** Keeps the library in step with the folders on disk, then analyses whatever
    has not been analysed.

    Two passes on one background thread. The first walks the folders and reads
    tags, which is fast, so the browser fills up in seconds. The second decodes
    each new track and runs the analyser, which is slow, so it runs at low
    priority and can be stopped and picked up again later: every result is
    written as it is found.
*/
class LibraryScanner final : private juce::Thread
{
public:
    explicit LibraryScanner (Library& libraryToUse);
    ~LibraryScanner() override;

    /** Starts a run, or asks a running one to go round again when it finishes. */
    void start();
    void stop();
    bool isRunning() const { return isThreadRunning(); }

    struct Progress
    {
        bool running = false;
        bool scanning = false;        ///< in the folder pass rather than the analysis pass
        int filesFound = 0;
        int filesScanned = 0;
        int tracksToAnalyse = 0;
        int tracksAnalysed = 0;
        juce::String currentFile;
    };

    Progress getProgress() const;

    /** Called from the scanner thread whenever the numbers move. */
    std::function<void()> onProgress;

private:
    void run() override;
    void scanFolders();
    void pruneMissing();
    void analysePending();
    void report();

    Library& library;
    juce::AudioFormatManager formatManager;   // the same formats the decks register

    mutable std::mutex progressMutex;
    Progress progress;
    std::atomic<bool> runAgain { false };
};

} // namespace opendj
