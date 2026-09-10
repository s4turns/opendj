/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/AnalysisCache.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <mutex>
#include <optional>
#include <vector>

struct sqlite3;

namespace opendj
{

/** One row of the track table. */
struct TrackRecord
{
    juce::int64 id = 0;
    juce::File file;

    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String genre;
    juce::String key;

    double durationSeconds = 0.0;
    double bpm = 0.0;
    double firstBeatSeconds = 0.0;
    float tempoConfidence = 0.0f;
    bool analysed = false;

    juce::int64 fileSize = 0;
    juce::int64 fileModified = 0;   ///< milliseconds since the epoch

    /** Title, or the file name when there is none. */
    juce::String displayTitle() const;
};

/** What to list and in what order. */
struct LibraryQuery
{
    enum class SortBy { title, artist, album, bpm, duration, added };

    juce::String search;           ///< matched against title, artist, album and path
    juce::File folder;             ///< limit to files under this folder, if set
    SortBy sortBy = SortBy::artist;
    bool ascending = true;
    int limit = 10000;
};

/** The track database: an SQLite file with the folders being watched, every
    track found in them, what their tags say, and what the analyser found.

    Every method takes the same lock, so it can be called from the message
    thread, the scanner and the engine's loader threads alike. None of them are
    the audio thread and none of them should be.
*/
class Library final : public AnalysisCache
{
public:
    /** Bump this when the analyser changes enough that old results are wrong.
        Every track then quietly goes back into the queue.

        2: the analyser now detects key as well as tempo, so tracks analysed by
        version 1 have an empty key column that only a re-analysis will fill. */
    static constexpr int analysisVersion = 2;

    Library();
    ~Library() override;

    /** Where the database lives for this user. */
    static juce::File defaultFile();

    juce::Result open (const juce::File& databaseFile);
    void close();
    bool isOpen() const;

    /** Called after anything changes, from whichever thread changed it. */
    std::function<void()> onChanged;

    //==========================================================================
    // Folders
    //==========================================================================

    bool addFolder (const juce::File& folder);
    bool removeFolder (const juce::File& folder);
    std::vector<juce::File> getFolders() const;

    //==========================================================================
    // Tracks
    //==========================================================================

    std::optional<TrackRecord> findTrack (const juce::File& file) const;

    /** Inserts or updates by path. The tags and file stamps are always
        written; the analysis columns only when the record says analysed. A
        file whose size or date changed loses its analysis. */
    juce::int64 upsertTrack (const TrackRecord& record);

    bool removeTrack (const juce::File& file);

    std::vector<TrackRecord> query (const LibraryQuery& query) const;

    /** Tracks the analyser has not yet seen with the current version. */
    std::vector<juce::File> tracksNeedingAnalysis (int limit) const;

    /** Takes a file out of the analysis queue for good, after it failed to decode. */
    void markUnreadable (const juce::File& file);

    int countTracks() const;
    int countAnalysed() const;

    //==========================================================================
    // AnalysisCache
    //==========================================================================

    std::optional<KnownTrack> lookup (const juce::File& file) override;
    void store (const juce::File& file, const TrackAnalysis& analysis, double durationSeconds) override;

private:
    class Statement;

    bool execute (const char* sql) const;
    juce::Result createSchema();
    static TrackRecord recordFromRow (Statement& statement);
    static bool isFileUnchanged (const TrackRecord& record, const juce::File& file);
    void changed();

    mutable std::mutex mutex;
    sqlite3* database = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Library)
};

} // namespace opendj
