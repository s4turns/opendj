/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "library/Library.h"

#include "library/TagReader.h"

#include <sqlite3.h>

namespace opendj
{

//==============================================================================
// A prepared statement that finalises itself. Thin on purpose: the point is
// that no raw sqlite3_stmt escapes into the rest of the file.
//==============================================================================

class Library::Statement
{
public:
    Statement (sqlite3* db, const char* sql)
    {
        if (db != nullptr)
            sqlite3_prepare_v2 (db, sql, -1, &statement, nullptr);
    }

    ~Statement()
    {
        if (statement != nullptr)
            sqlite3_finalize (statement);
    }

    bool isValid() const noexcept { return statement != nullptr; }

    Statement& bind (int index, const juce::String& value)
    {
        sqlite3_bind_text (statement, index, value.toRawUTF8(), -1, SQLITE_TRANSIENT);
        return *this;
    }

    Statement& bind (int index, juce::int64 value)
    {
        sqlite3_bind_int64 (statement, index, value);
        return *this;
    }

    Statement& bind (int index, int value)      { return bind (index, (juce::int64) value); }
    Statement& bind (int index, double value)   { sqlite3_bind_double (statement, index, value); return *this; }

    /** Advances to the next row. False when there are no more, or on error. */
    bool step()
    {
        return statement != nullptr && sqlite3_step (statement) == SQLITE_ROW;
    }

    /** Runs a statement that returns no rows. */
    bool run()
    {
        return statement != nullptr && sqlite3_step (statement) == SQLITE_DONE;
    }

    juce::int64 getInt64 (int column) const  { return sqlite3_column_int64 (statement, column); }
    int getInt (int column) const            { return sqlite3_column_int (statement, column); }
    double getDouble (int column) const      { return sqlite3_column_double (statement, column); }

    juce::String getText (int column) const
    {
        const auto* text = reinterpret_cast<const char*> (sqlite3_column_text (statement, column));
        return text != nullptr ? juce::String::fromUTF8 (text) : juce::String();
    }

private:
    sqlite3_stmt* statement = nullptr;

    JUCE_DECLARE_NON_COPYABLE (Statement)
};

namespace
{
    // Every column of the track table, in the order recordFromRow() reads them.
    constexpr const char* trackColumns =
        "id, path, title, artist, album, genre, key, duration_seconds, bpm, "
        "first_beat_seconds, tempo_confidence, analysis_version, file_size, file_modified";

    constexpr int schemaVersion = 1;

    juce::String pathOf (const juce::File& file)
    {
        return file.getFullPathName();
    }

    /** The prefix every path inside a folder starts with. */
    juce::String folderPrefix (const juce::File& folder)
    {
        return folder.getFullPathName() + juce::File::getSeparatorString();
    }

    juce::String likeEscaped (const juce::String& text)
    {
        return text.replace ("\\", "\\\\").replace ("%", "\\%").replace ("_", "\\_");
    }
}

//==============================================================================

juce::String TrackRecord::displayTitle() const
{
    return title.isNotEmpty() ? title : file.getFileNameWithoutExtension();
}

//==============================================================================

Library::Library() = default;

Library::~Library()
{
    close();
}

juce::File Library::defaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("OpenDJ")
               .getChildFile ("library.sqlite");
}

juce::Result Library::open (const juce::File& databaseFile)
{
    std::lock_guard<std::mutex> lock (mutex);

    if (database != nullptr)
    {
        sqlite3_close (database);
        database = nullptr;
    }

    databaseFile.getParentDirectory().createDirectory();

    // FULLMUTEX so the connection itself is safe to share; the class lock on
    // top of it is what keeps a find-then-update pair together.
    const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    if (sqlite3_open_v2 (databaseFile.getFullPathName().toRawUTF8(), &database, flags, nullptr) != SQLITE_OK)
    {
        const juce::String message (database != nullptr ? sqlite3_errmsg (database) : "could not open");
        sqlite3_close (database);
        database = nullptr;
        return juce::Result::fail ("Could not open " + databaseFile.getFullPathName() + ": " + message);
    }

    execute ("PRAGMA journal_mode = WAL");
    execute ("PRAGMA synchronous = NORMAL");
    execute ("PRAGMA busy_timeout = 5000");

    return createSchema();
}

void Library::close()
{
    std::lock_guard<std::mutex> lock (mutex);

    if (database != nullptr)
    {
        sqlite3_close (database);
        database = nullptr;
    }
}

bool Library::isOpen() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return database != nullptr;
}

bool Library::execute (const char* sql) const
{
    return database != nullptr && sqlite3_exec (database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

juce::Result Library::createSchema()
{
    const auto ok = execute (
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL);"

        "CREATE TABLE IF NOT EXISTS folders ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT NOT NULL UNIQUE);"

        "CREATE TABLE IF NOT EXISTS tracks ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT NOT NULL UNIQUE,"
        "  file_size INTEGER NOT NULL DEFAULT 0,"
        "  file_modified INTEGER NOT NULL DEFAULT 0,"
        "  title TEXT NOT NULL DEFAULT '',"
        "  artist TEXT NOT NULL DEFAULT '',"
        "  album TEXT NOT NULL DEFAULT '',"
        "  genre TEXT NOT NULL DEFAULT '',"
        "  key TEXT NOT NULL DEFAULT '',"
        "  duration_seconds REAL NOT NULL DEFAULT 0,"
        "  bpm REAL NOT NULL DEFAULT 0,"
        "  first_beat_seconds REAL NOT NULL DEFAULT 0,"
        "  tempo_confidence REAL NOT NULL DEFAULT 0,"
        "  analysis_version INTEGER NOT NULL DEFAULT 0,"   // 0 pending, -1 unreadable
        "  added_at INTEGER NOT NULL DEFAULT 0,"
        "  last_played_at INTEGER NOT NULL DEFAULT 0);"

        "CREATE INDEX IF NOT EXISTS tracks_by_title ON tracks (title COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_by_artist ON tracks (artist COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_pending ON tracks (analysis_version);");

    if (! ok)
        return juce::Result::fail (juce::String ("Could not create the library schema: ") + sqlite3_errmsg (database));

    Statement version (database, "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)");
    version.bind (1, juce::String (schemaVersion)).run();

    return juce::Result::ok();
}

void Library::changed()
{
    if (onChanged != nullptr)
        onChanged();
}

//==============================================================================
// Folders
//==============================================================================

bool Library::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return false;

    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement insert (database, "INSERT OR IGNORE INTO folders (path) VALUES (?)");

        if (! insert.bind (1, pathOf (folder)).run())
            return false;
    }

    changed();
    return true;
}

bool Library::removeFolder (const juce::File& folder)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement remove (database, "DELETE FROM folders WHERE path = ?");

        if (! remove.bind (1, pathOf (folder)).run() || sqlite3_changes (database) == 0)
            return false;
    }

    changed();
    return true;
}

std::vector<juce::File> Library::getFolders() const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::vector<juce::File> folders;

    Statement select (database, "SELECT path FROM folders ORDER BY path");

    while (select.step())
        folders.emplace_back (select.getText (0));

    return folders;
}

//==============================================================================
// Tracks
//==============================================================================

TrackRecord Library::recordFromRow (Statement& row)
{
    TrackRecord record;
    record.id = row.getInt64 (0);
    record.file = juce::File (row.getText (1));
    record.title = row.getText (2);
    record.artist = row.getText (3);
    record.album = row.getText (4);
    record.genre = row.getText (5);
    record.key = row.getText (6);
    record.durationSeconds = row.getDouble (7);
    record.bpm = row.getDouble (8);
    record.firstBeatSeconds = row.getDouble (9);
    record.tempoConfidence = (float) row.getDouble (10);
    record.analysed = row.getInt (11) == analysisVersion;
    record.fileSize = row.getInt64 (12);
    record.fileModified = row.getInt64 (13);
    return record;
}

bool Library::isFileUnchanged (const TrackRecord& record, const juce::File& file)
{
    return record.fileSize == file.getSize()
        && record.fileModified == file.getLastModificationTime().toMilliseconds();
}

std::optional<TrackRecord> Library::findTrack (const juce::File& file) const
{
    std::lock_guard<std::mutex> lock (mutex);

    Statement select (database, ("SELECT " + juce::String (trackColumns) + " FROM tracks WHERE path = ?").toRawUTF8());
    select.bind (1, pathOf (file));

    if (! select.step())
        return std::nullopt;

    return recordFromRow (select);
}

juce::int64 Library::upsertTrack (const TrackRecord& record)
{
    juce::int64 id = 0;

    {
        std::lock_guard<std::mutex> lock (mutex);

        Statement existing (database, "SELECT id, file_size, file_modified FROM tracks WHERE path = ?");
        existing.bind (1, pathOf (record.file));

        if (existing.step())
        {
            id = existing.getInt64 (0);

            // A file that changed on disk is a different recording as far as the
            // beat grid is concerned, so the analysis goes unless the caller is
            // supplying a fresh one.
            const auto fileChanged = existing.getInt64 (1) != record.fileSize
                                  || existing.getInt64 (2) != record.fileModified;

            Statement update (database,
                record.analysed
                    ? "UPDATE tracks SET file_size = ?, file_modified = ?, title = ?, artist = ?, album = ?,"
                      " genre = ?, key = ?, duration_seconds = ?, bpm = ?, first_beat_seconds = ?,"
                      " tempo_confidence = ?, analysis_version = ? WHERE id = ?"
                    : fileChanged
                        ? "UPDATE tracks SET file_size = ?, file_modified = ?, title = ?, artist = ?, album = ?,"
                          " genre = ?, key = ?, duration_seconds = ?, bpm = 0, first_beat_seconds = 0,"
                          " tempo_confidence = 0, analysis_version = 0 WHERE id = ? AND ? = ? AND ? = ? AND ? = ?"
                        : "UPDATE tracks SET file_size = ?, file_modified = ?, title = ?, artist = ?, album = ?,"
                          " genre = ?, key = ?, duration_seconds = CASE WHEN ? > 0 THEN ? ELSE duration_seconds END"
                          " WHERE id = ? AND ? = ? AND ? = ?");

            update.bind (1, record.fileSize).bind (2, record.fileModified)
                  .bind (3, record.title).bind (4, record.artist).bind (5, record.album)
                  .bind (6, record.genre).bind (7, record.key);

            if (record.analysed)
            {
                update.bind (8, record.durationSeconds).bind (9, record.bpm)
                      .bind (10, record.firstBeatSeconds).bind (11, (double) record.tempoConfidence)
                      .bind (12, analysisVersion).bind (13, id);
            }
            else if (fileChanged)
            {
                // The trailing "? = ?" pairs exist only so the same bind count
                // works for every variant; they are always true.
                update.bind (8, record.durationSeconds).bind (9, id)
                      .bind (10, 1).bind (11, 1).bind (12, 1).bind (13, 1).bind (14, 1).bind (15, 1);
            }
            else
            {
                update.bind (8, record.durationSeconds).bind (9, record.durationSeconds).bind (10, id)
                      .bind (11, 1).bind (12, 1).bind (13, 1).bind (14, 1);
            }

            update.run();
        }
        else
        {
            Statement insert (database,
                "INSERT INTO tracks (path, file_size, file_modified, title, artist, album, genre, key,"
                " duration_seconds, bpm, first_beat_seconds, tempo_confidence, analysis_version, added_at)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

            insert.bind (1, pathOf (record.file)).bind (2, record.fileSize).bind (3, record.fileModified)
                  .bind (4, record.title).bind (5, record.artist).bind (6, record.album)
                  .bind (7, record.genre).bind (8, record.key).bind (9, record.durationSeconds)
                  .bind (10, record.analysed ? record.bpm : 0.0)
                  .bind (11, record.analysed ? record.firstBeatSeconds : 0.0)
                  .bind (12, record.analysed ? (double) record.tempoConfidence : 0.0)
                  .bind (13, record.analysed ? analysisVersion : 0)
                  .bind (14, juce::Time::currentTimeMillis());

            if (insert.run())
                id = sqlite3_last_insert_rowid (database);
        }
    }

    if (id != 0)
        changed();

    return id;
}

bool Library::removeTrack (const juce::File& file)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement remove (database, "DELETE FROM tracks WHERE path = ?");

        if (! remove.bind (1, pathOf (file)).run() || sqlite3_changes (database) == 0)
            return false;
    }

    changed();
    return true;
}

std::vector<TrackRecord> Library::query (const LibraryQuery& query) const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::vector<TrackRecord> results;

    juce::String sql = "SELECT " + juce::String (trackColumns) + " FROM tracks WHERE 1 = 1";
    juce::StringArray terms;

    if (query.folder != juce::File())
        sql << " AND substr(path, 1, ?) = ?";

    // Every word must match somewhere, which is how a search box is expected
    // to behave: "boys noize" finds the artist, "noize 2007" narrows it down.
    terms.addTokens (query.search, " \t", "\"");
    terms.removeEmptyStrings();

    for (int i = 0; i < terms.size(); ++i)
        sql << " AND (title LIKE ? ESCAPE '\\' OR artist LIKE ? ESCAPE '\\'"
               " OR album LIKE ? ESCAPE '\\' OR path LIKE ? ESCAPE '\\')";

    const auto column = [&query]
    {
        switch (query.sortBy)
        {
            case LibraryQuery::SortBy::title:    return "title COLLATE NOCASE";
            case LibraryQuery::SortBy::artist:   return "artist COLLATE NOCASE";
            case LibraryQuery::SortBy::album:    return "album COLLATE NOCASE";
            case LibraryQuery::SortBy::bpm:      return "bpm";
            case LibraryQuery::SortBy::duration: return "duration_seconds";
            case LibraryQuery::SortBy::added:    return "added_at";
        }

        return "title COLLATE NOCASE";
    }();

    sql << " ORDER BY " << column << (query.ascending ? " ASC" : " DESC")
        << ", title COLLATE NOCASE ASC LIMIT ?";

    Statement select (database, sql.toRawUTF8());

    if (! select.isValid())
        return results;

    auto index = 1;

    if (query.folder != juce::File())
    {
        const auto prefix = folderPrefix (query.folder);
        select.bind (index++, (int) prefix.getNumBytesAsUTF8()).bind (index++, prefix);
    }

    for (const auto& term : terms)
    {
        const auto pattern = "%" + likeEscaped (term.unquoted()) + "%";

        for (int i = 0; i < 4; ++i)
            select.bind (index++, pattern);
    }

    select.bind (index, juce::jmax (1, query.limit));

    while (select.step())
        results.push_back (recordFromRow (select));

    return results;
}

std::vector<juce::File> Library::tracksNeedingAnalysis (int limit) const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::vector<juce::File> files;

    Statement select (database,
        "SELECT path FROM tracks WHERE analysis_version >= 0 AND analysis_version < ?"
        " ORDER BY added_at, id LIMIT ?");
    select.bind (1, analysisVersion).bind (2, juce::jmax (1, limit));

    while (select.step())
        files.emplace_back (select.getText (0));

    return files;
}

void Library::markUnreadable (const juce::File& file)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement update (database, "UPDATE tracks SET analysis_version = -1 WHERE path = ?");
        update.bind (1, pathOf (file)).run();
    }

    changed();
}

int Library::countTracks() const
{
    std::lock_guard<std::mutex> lock (mutex);
    Statement count (database, "SELECT COUNT(*) FROM tracks");
    return count.step() ? count.getInt (0) : 0;
}

int Library::countAnalysed() const
{
    std::lock_guard<std::mutex> lock (mutex);
    Statement count (database, "SELECT COUNT(*) FROM tracks WHERE analysis_version = ? OR analysis_version < 0");
    count.bind (1, analysisVersion);
    return count.step() ? count.getInt (0) : 0;
}

//==============================================================================
// AnalysisCache
//==============================================================================

std::optional<KnownTrack> Library::lookup (const juce::File& file)
{
    const auto record = findTrack (file);

    if (! record.has_value())
        return std::nullopt;

    KnownTrack known;
    known.title = record->title;
    known.artist = record->artist;

    // A result is only trusted for the file it was made from.
    known.analysed = record->analysed && isFileUnchanged (*record, file);
    known.bpm = record->bpm;
    known.firstBeatSeconds = record->firstBeatSeconds;
    known.tempoConfidence = record->tempoConfidence;

    return known;
}

void Library::store (const juce::File& file, const TrackAnalysis& analysis, double durationSeconds)
{
    auto record = findTrack (file).value_or (TrackRecord());

    if (record.id == 0)
    {
        // A track played from outside the watched folders is remembered too,
        // so its beat grid is there next time it is dragged in.
        TrackTags tags;
        TagReader::guessFromFileName (file, tags);
        record.file = file;
        record.title = tags.title;
        record.artist = tags.artist;
    }

    record.fileSize = file.getSize();
    record.fileModified = file.getLastModificationTime().toMilliseconds();
    record.durationSeconds = durationSeconds;
    record.bpm = analysis.bpm;
    record.firstBeatSeconds = analysis.firstBeatSeconds;
    record.tempoConfidence = analysis.confidence;

    // A key written into the file's tags was put there by a person, or at least
    // by software the owner of the collection chose. Detection fills the gap
    // where there is nothing; it does not overrule what is already claimed.
    if (analysis.hasKey() && record.key.isEmpty())
        record.key = analysis.key.toString();

    record.analysed = true;

    upsertTrack (record);
}

} // namespace opendj
