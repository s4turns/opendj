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
        "first_beat_seconds, tempo_confidence, analysis_version, file_size, file_modified, "
        "rating, colour, comment";

    // 2: rating, colour and comment columns, and the hot_cues table.
    // 3: a name column on hot_cues.
    constexpr int schemaVersion = 3;

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

        "CREATE TABLE IF NOT EXISTS playlists ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL UNIQUE COLLATE NOCASE);"

        "CREATE TABLE IF NOT EXISTS playlist_tracks ("
        "  playlist_id INTEGER NOT NULL REFERENCES playlists (id) ON DELETE CASCADE,"
        "  position INTEGER NOT NULL,"
        "  path TEXT NOT NULL,"
        "  PRIMARY KEY (playlist_id, position));"

        "CREATE TABLE IF NOT EXISTS hot_cues ("
        "  path TEXT NOT NULL,"
        "  slot INTEGER NOT NULL,"
        "  seconds REAL NOT NULL,"
        "  name TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY (path, slot));"

        "CREATE INDEX IF NOT EXISTS tracks_by_title ON tracks (title COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_by_artist ON tracks (artist COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_pending ON tracks (analysis_version);");

    // Columns added after the first release. CREATE TABLE IF NOT EXISTS leaves
    // an older database's table as it was, so they are added to it here.
    const auto upgraded = ok
        && addColumnIfMissing ("tracks", "rating", "INTEGER NOT NULL DEFAULT 0")
        && addColumnIfMissing ("tracks", "colour", "INTEGER NOT NULL DEFAULT 0")
        && addColumnIfMissing ("tracks", "comment", "TEXT NOT NULL DEFAULT ''")
        && addColumnIfMissing ("hot_cues", "name", "TEXT NOT NULL DEFAULT ''")
        && addColumnIfMissing ("tracks", "grid_edited", "INTEGER NOT NULL DEFAULT 0");

    if (! upgraded)
        return juce::Result::fail (juce::String ("Could not create the library schema: ") + sqlite3_errmsg (database));

    Statement version (database, "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)");
    version.bind (1, juce::String (schemaVersion)).run();

    return juce::Result::ok();
}

bool Library::addColumnIfMissing (const char* table, const char* column, const char* definition)
{
    Statement columns (database, (juce::String ("PRAGMA table_info (") + table + ")").toRawUTF8());

    while (columns.step())
        if (columns.getText (1) == column)
            return true;

    return execute ((juce::String ("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + definition).toRawUTF8());
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

namespace
{
    // Tracks under the folder in the first parameter that no other watched
    // folder holds. substr() and length() both count characters, so a prefix
    // with an å in it compares correctly.
    constexpr const char* onlyInFolder =
        " FROM tracks WHERE substr(path, 1, length(?1)) = ?1"
        " AND NOT EXISTS (SELECT 1 FROM folders"
        "                 WHERE folders.path <> ?2"
        "                 AND substr(tracks.path, 1, length(folders.path) + 1) = folders.path || ?3)";
}

bool Library::removeFolder (const juce::File& folder)
{
    {
        std::lock_guard<std::mutex> lock (mutex);

        if (! execute ("BEGIN"))
            return false;

        Statement remove (database, "DELETE FROM folders WHERE path = ?");
        auto ok = remove.bind (1, pathOf (folder)).run() && sqlite3_changes (database) > 0;

        if (ok)
        {
            Statement tracks (database, (juce::String ("DELETE") + onlyInFolder).toRawUTF8());
            ok = tracks.bind (1, folderPrefix (folder)).bind (2, pathOf (folder))
                       .bind (3, juce::File::getSeparatorString()).run();
        }

        execute (ok ? "COMMIT" : "ROLLBACK");

        if (! ok)
            return false;
    }

    changed();
    return true;
}

bool Library::isWatched (const juce::File& folder) const
{
    std::lock_guard<std::mutex> lock (mutex);
    Statement select (database, "SELECT 1 FROM folders WHERE path = ?");
    return select.bind (1, pathOf (folder)).step();
}

int Library::countTracksOnlyIn (const juce::File& folder) const
{
    std::lock_guard<std::mutex> lock (mutex);
    Statement count (database, (juce::String ("SELECT COUNT(*)") + onlyInFolder).toRawUTF8());
    count.bind (1, folderPrefix (folder)).bind (2, pathOf (folder)).bind (3, juce::File::getSeparatorString());
    return count.step() ? count.getInt (0) : 0;
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
    record.rating = row.getInt (14);
    record.colour = (juce::uint32) row.getInt64 (15);
    record.comment = row.getText (16);
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
                    // A grid set by hand outranks any analysis or import:
                    // the person who fixed it has listened to the track.
                    ? "UPDATE tracks SET file_size = ?, file_modified = ?, title = ?, artist = ?, album = ?,"
                      " genre = ?, key = ?, duration_seconds = ?,"
                      " bpm = CASE WHEN grid_edited THEN bpm ELSE ? END,"
                      " first_beat_seconds = CASE WHEN grid_edited THEN first_beat_seconds ELSE ? END,"
                      " tempo_confidence = CASE WHEN grid_edited THEN tempo_confidence ELSE ? END,"
                      " analysis_version = ? WHERE id = ?"
                    : fileChanged
                        ? "UPDATE tracks SET file_size = ?, file_modified = ?, title = ?, artist = ?, album = ?,"
                          " genre = ?, key = ?, duration_seconds = ?, bpm = 0, first_beat_seconds = 0,"
                          " tempo_confidence = 0, analysis_version = 0, grid_edited = 0"
                          " WHERE id = ? AND ? = ? AND ? = ? AND ? = ?"
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
        sql << " AND substr(path, 1, length(?1)) = ?1";

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
        // Counted in characters, as substr() counts them.
        select.bind (index++, folderPrefix (query.folder));
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

//==============================================================================
// Playlists
//==============================================================================

juce::int64 Library::createPlaylist (const juce::String& name)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty())
        return 0;

    juce::int64 id = 0;

    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement insert (database, "INSERT INTO playlists (name) VALUES (?)");

        if (insert.bind (1, trimmed).run())
            id = sqlite3_last_insert_rowid (database);
    }

    if (id != 0)
        changed();

    return id;
}

bool Library::renamePlaylist (juce::int64 id, const juce::String& name)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty())
        return false;

    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement update (database, "UPDATE playlists SET name = ? WHERE id = ?");
        ok = update.bind (1, trimmed).bind (2, id).run() && sqlite3_changes (database) > 0;
    }

    if (ok)
        changed();

    return ok;
}

bool Library::deletePlaylist (juce::int64 id)
{
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);

        // Both deleted by hand: foreign keys are off unless asked for, and a
        // playlist's rows must not outlive it and reappear under a reused id.
        Statement entries (database, "DELETE FROM playlist_tracks WHERE playlist_id = ?");
        entries.bind (1, id).run();

        Statement remove (database, "DELETE FROM playlists WHERE id = ?");
        ok = remove.bind (1, id).run() && sqlite3_changes (database) > 0;
    }

    if (ok)
        changed();

    return ok;
}

std::vector<PlaylistInfo> Library::getPlaylists() const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::vector<PlaylistInfo> playlists;

    Statement select (database,
        "SELECT p.id, p.name, COUNT(t.position) FROM playlists p "
        "LEFT JOIN playlist_tracks t ON t.playlist_id = p.id "
        "GROUP BY p.id ORDER BY p.name COLLATE NOCASE");

    while (select.step())
        playlists.push_back ({ select.getInt64 (0), select.getText (1), select.getInt (2) });

    return playlists;
}

std::vector<juce::File> Library::readPlaylistFiles (juce::int64 id) const
{
    std::vector<juce::File> files;

    Statement select (database, "SELECT path FROM playlist_tracks WHERE playlist_id = ? ORDER BY position");
    select.bind (1, id);

    while (select.step())
        files.emplace_back (select.getText (0));

    return files;
}

bool Library::writePlaylistFiles (juce::int64 id, const std::vector<juce::File>& files)
{
    if (! execute ("BEGIN"))
        return false;

    Statement clear (database, "DELETE FROM playlist_tracks WHERE playlist_id = ?");
    auto ok = clear.bind (1, id).run();

    for (size_t i = 0; ok && i < files.size(); ++i)
    {
        Statement insert (database, "INSERT INTO playlist_tracks (playlist_id, position, path) VALUES (?, ?, ?)");
        ok = insert.bind (1, id).bind (2, (int) i).bind (3, pathOf (files[i])).run();
    }

    execute (ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

std::vector<juce::File> Library::getPlaylistFiles (juce::int64 id) const
{
    std::lock_guard<std::mutex> lock (mutex);
    return readPlaylistFiles (id);
}

bool Library::addToPlaylist (juce::int64 id, const std::vector<juce::File>& additions)
{
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);

        Statement exists (database, "SELECT 1 FROM playlists WHERE id = ?");

        if (! exists.bind (1, id).step())
            return false;

        auto files = readPlaylistFiles (id);
        files.insert (files.end(), additions.begin(), additions.end());
        ok = writePlaylistFiles (id, files);
    }

    if (ok)
        changed();

    return ok;
}

bool Library::removeFromPlaylist (juce::int64 id, int position)
{
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);
        auto files = readPlaylistFiles (id);

        if (! juce::isPositiveAndBelow (position, (int) files.size()))
            return false;

        files.erase (files.begin() + position);
        ok = writePlaylistFiles (id, files);
    }

    if (ok)
        changed();

    return ok;
}

bool Library::movePlaylistTrack (juce::int64 id, int from, int to)
{
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);
        auto files = readPlaylistFiles (id);
        const auto size = (int) files.size();

        if (! juce::isPositiveAndBelow (from, size) || ! juce::isPositiveAndBelow (to, size))
            return false;

        const auto moved = files[(size_t) from];
        files.erase (files.begin() + from);
        files.insert (files.begin() + to, moved);
        ok = writePlaylistFiles (id, files);
    }

    if (ok)
        changed();

    return ok;
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

bool Library::setTrackDetails (const juce::File& file, int rating, juce::uint32 colour, const juce::String& comment)
{
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock (mutex);
        Statement update (database, "UPDATE tracks SET rating = ?, colour = ?, comment = ? WHERE path = ?");
        ok = update.bind (1, juce::jlimit (0, 5, rating)).bind (2, (juce::int64) colour)
                   .bind (3, comment).bind (4, pathOf (file)).run()
             && sqlite3_changes (database) > 0;
    }

    if (ok)
        changed();

    return ok;
}

//==============================================================================
// Hot cues
//==============================================================================

HotCues Library::readHotCues (const juce::File& file) const
{
    auto cues = noHotCues();

    Statement select (database, "SELECT slot, seconds FROM hot_cues WHERE path = ?");
    select.bind (1, pathOf (file));

    while (select.step())
        if (const auto slot = select.getInt (0); juce::isPositiveAndBelow (slot, numHotCues))
            cues[(size_t) slot] = select.getDouble (1);

    return cues;
}

HotCues Library::getHotCues (const juce::File& file) const
{
    std::lock_guard<std::mutex> lock (mutex);
    return readHotCues (file);
}

bool Library::setHotCues (const juce::File& file, const HotCues& cues)
{
    std::lock_guard<std::mutex> lock (mutex);

    if (! execute ("BEGIN"))
        return false;

    auto ok = true;

    // A set slot has its position written and keeps any name it already had; an
    // empty slot has its row removed. A plain delete-and-reinsert would drop the
    // names, and this runs on every pad press.
    for (int slot = 0; ok && slot < numHotCues; ++slot)
    {
        if (cues[(size_t) slot] < 0.0)
        {
            Statement remove (database, "DELETE FROM hot_cues WHERE path = ? AND slot = ?");
            ok = remove.bind (1, pathOf (file)).bind (2, slot).run();
        }
        else
        {
            Statement upsert (database,
                "INSERT INTO hot_cues (path, slot, seconds) VALUES (?, ?, ?) "
                "ON CONFLICT (path, slot) DO UPDATE SET seconds = excluded.seconds");
            ok = upsert.bind (1, pathOf (file)).bind (2, slot).bind (3, cues[(size_t) slot]).run();
        }
    }

    execute (ok ? "COMMIT" : "ROLLBACK");
    return ok;
}

HotCueNames Library::readHotCueNames (const juce::File& file) const
{
    auto names = noHotCueNames();

    Statement select (database, "SELECT slot, name FROM hot_cues WHERE path = ?");
    select.bind (1, pathOf (file));

    while (select.step())
        if (const auto slot = select.getInt (0); juce::isPositiveAndBelow (slot, numHotCues))
            names[(size_t) slot] = select.getText (1);

    return names;
}

HotCueNames Library::getHotCueNames (const juce::File& file)
{
    std::lock_guard<std::mutex> lock (mutex);
    return readHotCueNames (file);
}

void Library::setHotCueName (const juce::File& file, int slot, double seconds, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (slot, numHotCues))
        return;

    std::lock_guard<std::mutex> lock (mutex);

    // Upserting the position too means a name sticks even if it is typed before
    // the set that created the cue has reached the library.
    Statement upsert (database,
        "INSERT INTO hot_cues (path, slot, seconds, name) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (path, slot) DO UPDATE SET name = excluded.name");
    upsert.bind (1, pathOf (file)).bind (2, slot).bind (3, seconds).bind (4, name).run();
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
    known.hotCues = getHotCues (file);

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

void Library::storeHotCues (const juce::File& file, const HotCues& cues)
{
    setHotCues (file, cues);
}

void Library::storeBeatGrid (const juce::File& file, double bpm, double firstBeatSeconds)
{
    {
        std::lock_guard<std::mutex> lock (mutex);

        Statement update (database,
            "UPDATE tracks SET bpm = ?, first_beat_seconds = ?, tempo_confidence = 1, grid_edited = 1,"
            " analysis_version = ? WHERE path = ?");
        update.bind (1, bpm).bind (2, firstBeatSeconds).bind (3, analysisVersion).bind (4, pathOf (file));

        if (! update.run() || sqlite3_changes (database) == 0)
            return;
    }

    changed();   // the browser's BPM column
}

} // namespace opendj
