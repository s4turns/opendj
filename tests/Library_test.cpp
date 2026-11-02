/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "library/Library.h"

#include <sqlite3.h>

using Catch::Matchers::WithinAbs;

namespace
{
    /** A fresh database in a temporary folder, with a couple of real files in
        it so file sizes and dates mean something. */
    struct Fixture
    {
        Fixture()
        {
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("opendj-library-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            REQUIRE (folder.createDirectory());

            REQUIRE (library.open (folder.getChildFile ("library.sqlite")).wasOk());

            first = folder.getChildFile ("Boys Noize - Yeah.mp3");
            second = folder.getChildFile ("sub").getChildFile ("Plastikman - Spastik.wav");
            second.getParentDirectory().createDirectory();
            REQUIRE (first.replaceWithText ("not really audio"));
            REQUIRE (second.replaceWithText ("nor this"));
        }

        ~Fixture()
        {
            library.close();
            folder.deleteRecursively();
        }

        opendj::TrackRecord recordFor (const juce::File& file, const juce::String& title, const juce::String& artist)
        {
            opendj::TrackRecord record;
            record.file = file;
            record.title = title;
            record.artist = artist;
            record.durationSeconds = 300.0;
            record.fileSize = file.getSize();
            record.fileModified = file.getLastModificationTime().toMilliseconds();
            return record;
        }

        opendj::Library library;
        juce::File folder, first, second;
    };

    opendj::TrackAnalysis analysisAt (double bpm)
    {
        opendj::TrackAnalysis analysis;
        analysis.bpm = bpm;
        analysis.firstBeatSeconds = 0.25;
        analysis.confidence = 0.9f;
        return analysis;
    }
}

TEST_CASE ("a library remembers its folders", "[library]")
{
    Fixture fixture;

    REQUIRE (fixture.library.getFolders().empty());
    REQUIRE (fixture.library.addFolder (fixture.folder));
    REQUIRE (fixture.library.addFolder (fixture.folder));   // again is harmless
    REQUIRE (fixture.library.getFolders().size() == 1);
    REQUIRE (fixture.library.getFolders()[0] == fixture.folder);

    REQUIRE_FALSE (fixture.library.addFolder (fixture.first));   // a file is not a folder

    REQUIRE (fixture.library.removeFolder (fixture.folder));
    REQUIRE_FALSE (fixture.library.removeFolder (fixture.folder));
    REQUIRE (fixture.library.getFolders().empty());
}

TEST_CASE ("removing a folder drops the tracks only it holds", "[library]")
{
    Fixture fixture;
    auto& library = fixture.library;
    const auto sub = fixture.second.getParentDirectory();
    const auto outside = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("elsewhere.mp3");

    REQUIRE (library.addFolder (fixture.folder));
    REQUIRE (library.addFolder (sub));
    library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    library.upsertTrack (fixture.recordFor (fixture.second, "Spastik", "Plastikman"));
    library.upsertTrack (fixture.recordFor (outside, "Elsewhere", "Nobody"));

    // The sub folder's track is also under the outer folder, so it stays.
    REQUIRE (library.countTracksOnlyIn (sub) == 0);
    REQUIRE (library.removeFolder (sub));
    REQUIRE (library.countTracks() == 3);

    REQUIRE (library.isWatched (fixture.folder));
    REQUIRE (library.countTracksOnlyIn (fixture.folder) == 2);
    REQUIRE (library.removeFolder (fixture.folder));
    REQUIRE_FALSE (library.isWatched (fixture.folder));

    // A track remembered from outside every folder is left alone.
    REQUIRE (library.countTracks() == 1);
    REQUIRE (library.findTrack (outside).has_value());
}

TEST_CASE ("a folder with non-ASCII characters in its path filters its tracks", "[library]")
{
    Fixture fixture;
    const auto folder = fixture.folder.getChildFile (juce::CharPointer_UTF8 ("L\xc3\xa5tar"));
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile ("Track.mp3");
    REQUIRE (file.replaceWithText ("not really audio"));

    REQUIRE (fixture.library.addFolder (folder));
    fixture.library.upsertTrack (fixture.recordFor (file, "Track", "Someone"));
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));

    opendj::LibraryQuery inFolder;
    inFolder.folder = folder;
    const auto found = fixture.library.query (inFolder);
    REQUIRE (found.size() == 1);
    REQUIRE (found[0].file == file);

    REQUIRE (fixture.library.countTracksOnlyIn (folder) == 1);
}

TEST_CASE ("tracks round trip through the database", "[library]")
{
    Fixture fixture;

    const auto id = fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    REQUIRE (id > 0);

    const auto found = fixture.library.findTrack (fixture.first);
    REQUIRE (found.has_value());
    REQUIRE (found->id == id);
    REQUIRE (found->title == "Yeah");
    REQUIRE (found->artist == "Boys Noize");
    REQUIRE_THAT (found->durationSeconds, WithinAbs (300.0, 0.001));
    REQUIRE_FALSE (found->analysed);
    REQUIRE (found->fileSize == fixture.first.getSize());

    // Writing the same path again updates rather than duplicates.
    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah!", "Boys Noize")) == id);
    REQUIRE (fixture.library.countTracks() == 1);
    REQUIRE (fixture.library.findTrack (fixture.first)->title == "Yeah!");

    REQUIRE_FALSE (fixture.library.findTrack (fixture.second).has_value());
}

TEST_CASE ("searching matches every word against any field", "[library]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.upsertTrack (fixture.recordFor (fixture.second, "Spastik", "Plastikman"));

    opendj::LibraryQuery query;
    REQUIRE (fixture.library.query (query).size() == 2);

    query.search = "noize";
    auto results = fixture.library.query (query);
    REQUIRE (results.size() == 1);
    REQUIRE (results[0].title == "Yeah");

    query.search = "boys yeah";           // both words, different fields
    REQUIRE (fixture.library.query (query).size() == 1);

    query.search = "boys spastik";        // both words, different tracks
    REQUIRE (fixture.library.query (query).empty());

    query.search = "";
    query.folder = fixture.second.getParentDirectory();
    results = fixture.library.query (query);
    REQUIRE (results.size() == 1);
    REQUIRE (results[0].title == "Spastik");

    query.folder = juce::File();
    query.sortBy = opendj::LibraryQuery::SortBy::title;
    query.ascending = false;
    results = fixture.library.query (query);
    REQUIRE (results[0].title == "Yeah");
    REQUIRE (results[1].title == "Spastik");
}

TEST_CASE ("an analysis is cached and comes back for an unchanged file", "[library][cache]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));

    auto known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE (known->title == "Yeah");
    REQUIRE_FALSE (known->analysed);

    REQUIRE (fixture.library.tracksNeedingAnalysis (10).size() == 1);

    fixture.library.store (fixture.first, analysisAt (128.0), 301.5);

    known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE (known->analysed);
    REQUIRE_THAT (known->bpm, WithinAbs (128.0, 0.001));
    REQUIRE_THAT (known->firstBeatSeconds, WithinAbs (0.25, 0.001));
    REQUIRE_THAT (known->tempoConfidence, WithinAbs (0.9, 0.001));
    REQUIRE_THAT (fixture.library.findTrack (fixture.first)->durationSeconds, WithinAbs (301.5, 0.001));

    REQUIRE (fixture.library.tracksNeedingAnalysis (10).empty());
    REQUIRE (fixture.library.countAnalysed() == 1);
}

TEST_CASE ("a changed file loses its analysis", "[library][cache]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.store (fixture.first, analysisAt (128.0), 300.0);
    REQUIRE (fixture.library.lookup (fixture.first)->analysed);

    // The same path with different bytes behind it.
    REQUIRE (fixture.first.replaceWithText ("a longer replacement recording"));
    REQUIRE_FALSE (fixture.library.lookup (fixture.first)->analysed);

    // Once the scanner re-reads it, the queue has it again.
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    REQUIRE (fixture.library.tracksNeedingAnalysis (10).size() == 1);
    REQUIRE_THAT (fixture.library.findTrack (fixture.first)->bpm, WithinAbs (0.0, 0.001));
}

TEST_CASE ("a track loaded from outside the library is remembered", "[library][cache]")
{
    Fixture fixture;

    REQUIRE_FALSE (fixture.library.lookup (fixture.first).has_value());

    fixture.library.store (fixture.first, analysisAt (140.0), 300.0);

    const auto known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE (known->analysed);
    REQUIRE (known->title == "Yeah");          // from the file name
    REQUIRE (known->artist == "Boys Noize");
}

TEST_CASE ("an unreadable file leaves the queue and stays out", "[library]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.markUnreadable (fixture.first);

    REQUIRE (fixture.library.tracksNeedingAnalysis (10).empty());
    REQUIRE (fixture.library.countAnalysed() == 1);   // nothing left to do
    REQUIRE_FALSE (fixture.library.findTrack (fixture.first)->analysed);

    REQUIRE (fixture.library.removeTrack (fixture.first));
    REQUIRE (fixture.library.countTracks() == 0);
}

TEST_CASE ("playlists keep their order and survive a reopen", "[library][playlist]")
{
    Fixture f;
    const auto a = f.folder.getChildFile ("a.mp3");
    const auto b = f.folder.getChildFile ("b.mp3");
    const auto c = f.folder.getChildFile ("c.mp3");

    const auto id = f.library.createPlaylist ("  Friday stream ");
    REQUIRE (id != 0);

    // A name is unique without regard to case, and never empty.
    REQUIRE (f.library.createPlaylist ("friday STREAM") == 0);
    REQUIRE (f.library.createPlaylist ("   ") == 0);

    REQUIRE (f.library.addToPlaylist (id, { a, b }));
    REQUIRE (f.library.addToPlaylist (id, { c }));
    REQUIRE (f.library.getPlaylistFiles (id) == std::vector<juce::File> { a, b, c });

    const auto lists = f.library.getPlaylists();
    REQUIRE (lists.size() == 1);
    REQUIRE (lists[0].name == "Friday stream");
    REQUIRE (lists[0].numTracks == 3);

    f.library.close();
    REQUIRE (f.library.open (f.folder.getChildFile ("library.sqlite")).wasOk());
    REQUIRE (f.library.getPlaylistFiles (id) == std::vector<juce::File> { a, b, c });
}

TEST_CASE ("playlist tracks can be moved and removed", "[library][playlist]")
{
    Fixture f;
    const std::vector<juce::File> files {
        f.folder.getChildFile ("a.mp3"), f.folder.getChildFile ("b.mp3"),
        f.folder.getChildFile ("c.mp3"), f.folder.getChildFile ("d.mp3") };

    const auto id = f.library.createPlaylist ("set");
    REQUIRE (f.library.addToPlaylist (id, files));

    REQUIRE (f.library.movePlaylistTrack (id, 0, 2));
    REQUIRE (f.library.getPlaylistFiles (id) == std::vector<juce::File> { files[1], files[2], files[0], files[3] });

    REQUIRE (f.library.movePlaylistTrack (id, 3, 0));
    REQUIRE (f.library.getPlaylistFiles (id) == std::vector<juce::File> { files[3], files[1], files[2], files[0] });

    REQUIRE (f.library.removeFromPlaylist (id, 1));
    REQUIRE (f.library.getPlaylistFiles (id) == std::vector<juce::File> { files[3], files[2], files[0] });

    REQUIRE_FALSE (f.library.removeFromPlaylist (id, 7));
    REQUIRE_FALSE (f.library.movePlaylistTrack (id, 0, 9));
    REQUIRE_FALSE (f.library.addToPlaylist (id + 99, files));
}

TEST_CASE ("deleting a playlist removes its entries and renaming keeps them", "[library][playlist]")
{
    Fixture f;
    const auto file = f.folder.getChildFile ("a.mp3");

    const auto id = f.library.createPlaylist ("old name");
    REQUIRE (f.library.addToPlaylist (id, { file }));

    REQUIRE (f.library.renamePlaylist (id, "new name"));
    REQUIRE (f.library.getPlaylists()[0].name == "new name");
    REQUIRE (f.library.getPlaylistFiles (id).size() == 1);

    REQUIRE (f.library.deletePlaylist (id));
    REQUIRE (f.library.getPlaylists().empty());
    REQUIRE (f.library.getPlaylistFiles (id).empty());
}

TEST_CASE ("hot cues round trip by path and come back with the track", "[library][hotcue]")
{
    Fixture fixture;

    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize")) != 0);

    for (const auto cue : fixture.library.getHotCues (fixture.first))
        REQUIRE (cue < 0.0);

    auto cues = opendj::noHotCues();
    cues[0] = 12.5;
    cues[7] = 200.0;
    REQUIRE (fixture.library.setHotCues (fixture.first, cues));

    const auto stored = fixture.library.getHotCues (fixture.first);
    REQUIRE_THAT (stored[0], WithinAbs (12.5, 1.0e-9));
    REQUIRE_THAT (stored[7], WithinAbs (200.0, 1.0e-9));
    REQUIRE (stored[3] < 0.0);

    // The deck asks through the cache when it loads.
    const auto known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE_THAT (known->hotCues[0], WithinAbs (12.5, 1.0e-9));

    // Clearing a pad is writing every pad again without it.
    cues[0] = -1.0;
    fixture.library.storeHotCues (fixture.first, cues);
    REQUIRE (fixture.library.getHotCues (fixture.first)[0] < 0.0);
    REQUIRE_THAT (fixture.library.getHotCues (fixture.first)[7], WithinAbs (200.0, 1.0e-9));

    // Another file has its own.
    REQUIRE (fixture.library.getHotCues (fixture.second)[7] < 0.0);
}

TEST_CASE ("hot cue names round trip and survive the position being rewritten", "[library][hotcue]")
{
    Fixture fixture;

    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize")) != 0);

    auto cues = opendj::noHotCues();
    cues[0] = 12.5;
    cues[4] = 88.0;
    REQUIRE (fixture.library.setHotCues (fixture.first, cues));

    // A name is kept on the same row as the position.
    fixture.library.setHotCueName (fixture.first, 0, cues[0], "intro");
    fixture.library.setHotCueName (fixture.first, 4, cues[4], "drop");

    auto names = fixture.library.getHotCueNames (fixture.first);
    REQUIRE (names[0] == "intro");
    REQUIRE (names[4] == "drop");
    REQUIRE (names[1].isEmpty());

    // Rewriting the positions (what a pad press does) must not wipe the names.
    cues[0] = 15.0;
    fixture.library.storeHotCues (fixture.first, cues);
    REQUIRE_THAT (fixture.library.getHotCues (fixture.first)[0], WithinAbs (15.0, 1.0e-9));
    names = fixture.library.getHotCueNames (fixture.first);
    REQUIRE (names[0] == "intro");
    REQUIRE (names[4] == "drop");

    // Clearing a pad drops its name with it.
    cues[0] = -1.0;
    fixture.library.storeHotCues (fixture.first, cues);
    REQUIRE (fixture.library.getHotCueNames (fixture.first)[0].isEmpty());
    REQUIRE (fixture.library.getHotCueNames (fixture.first)[4] == "drop");
}

TEST_CASE ("hot cues outlive the track being removed and found again", "[library][hotcue]")
{
    Fixture fixture;

    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize")) != 0);

    auto cues = opendj::noHotCues();
    cues[2] = 30.0;
    REQUIRE (fixture.library.setHotCues (fixture.first, cues));

    REQUIRE (fixture.library.removeTrack (fixture.first));
    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize")) != 0);

    REQUIRE_THAT (fixture.library.getHotCues (fixture.first)[2], WithinAbs (30.0, 1.0e-9));
}

TEST_CASE ("rating, colour and comment are kept and survive a rescan", "[library]")
{
    Fixture fixture;

    REQUIRE_FALSE (fixture.library.setTrackDetails (fixture.first, 4, 0xffff007fu, "peak time"));   // not in the library yet

    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize")) != 0);
    REQUIRE (fixture.library.setTrackDetails (fixture.first, 4, 0xffff007fu, "peak time"));

    // The scanner writes tags over and over; it must not wipe these.
    REQUIRE (fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah (edit)", "Boys Noize")) != 0);

    const auto record = fixture.library.findTrack (fixture.first);
    REQUIRE (record.has_value());
    REQUIRE (record->title == "Yeah (edit)");
    REQUIRE (record->rating == 4);
    REQUIRE (record->colour == 0xffff007fu);
    REQUIRE (record->comment == "peak time");

    REQUIRE (fixture.library.setTrackDetails (fixture.first, 9, 0, {}));
    REQUIRE (fixture.library.findTrack (fixture.first)->rating == 5);
}

TEST_CASE ("a library made before ratings and hot cues is upgraded in place", "[library]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("opendj-library-upgrade-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    REQUIRE (folder.createDirectory());
    const auto databaseFile = folder.getChildFile ("library.sqlite");

    // The first schema's track table, with one track in it.
    {
        sqlite3* raw = nullptr;
        REQUIRE (sqlite3_open (databaseFile.getFullPathName().toRawUTF8(), &raw) == SQLITE_OK);
        REQUIRE (sqlite3_exec (raw,
            "CREATE TABLE tracks (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
            " file_size INTEGER NOT NULL DEFAULT 0, file_modified INTEGER NOT NULL DEFAULT 0,"
            " title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '',"
            " genre TEXT NOT NULL DEFAULT '', key TEXT NOT NULL DEFAULT '', duration_seconds REAL NOT NULL DEFAULT 0,"
            " bpm REAL NOT NULL DEFAULT 0, first_beat_seconds REAL NOT NULL DEFAULT 0,"
            " tempo_confidence REAL NOT NULL DEFAULT 0, analysis_version INTEGER NOT NULL DEFAULT 0,"
            " added_at INTEGER NOT NULL DEFAULT 0, last_played_at INTEGER NOT NULL DEFAULT 0);"
            "INSERT INTO tracks (path, title) VALUES ('/music/old.mp3', 'Old');",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close (raw);
    }

    {
        opendj::Library library;
        REQUIRE (library.open (databaseFile).wasOk());

        const auto old = library.findTrack (juce::File ("/music/old.mp3"));
        REQUIRE (old.has_value());
        REQUIRE (old->title == "Old");
        REQUIRE (old->rating == 0);

        REQUIRE (library.setTrackDetails (juce::File ("/music/old.mp3"), 3, 0, "kept"));

        auto cues = opendj::noHotCues();
        cues[1] = 4.0;
        REQUIRE (library.setHotCues (juce::File ("/music/old.mp3"), cues));
    }

    // And opening it again finds nothing more to do.
    {
        opendj::Library library;
        REQUIRE (library.open (databaseFile).wasOk());
        REQUIRE (library.findTrack (juce::File ("/music/old.mp3"))->comment == "kept");
        REQUIRE_THAT (library.getHotCues (juce::File ("/music/old.mp3"))[1], WithinAbs (4.0, 1.0e-9));
    }

    folder.deleteRecursively();
}

TEST_CASE ("a grid set by hand comes back and outranks the analyser", "[library][cache][grid]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.store (fixture.first, analysisAt (128.0), 300.0);

    fixture.library.storeBeatGrid (fixture.first, 127.5, 0.4);

    auto known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE (known->analysed);
    REQUIRE_THAT (known->bpm, WithinAbs (127.5, 1.0e-9));
    REQUIRE_THAT (known->firstBeatSeconds, WithinAbs (0.4, 1.0e-9));

    // A later analysis of the same file, or an import, must not undo it.
    fixture.library.store (fixture.first, analysisAt (64.0), 300.0);

    known = fixture.library.lookup (fixture.first);
    REQUIRE_THAT (known->bpm, WithinAbs (127.5, 1.0e-9));
    REQUIRE_THAT (known->firstBeatSeconds, WithinAbs (0.4, 1.0e-9));
    REQUIRE_THAT (fixture.library.findTrack (fixture.first)->bpm, WithinAbs (127.5, 1.0e-9));
}

TEST_CASE ("a changed file drops a grid set by hand", "[library][cache][grid]")
{
    Fixture fixture;
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.store (fixture.first, analysisAt (128.0), 300.0);
    fixture.library.storeBeatGrid (fixture.first, 127.5, 0.4);

    // A different recording now: its own analysis wins again.
    REQUIRE (fixture.first.replaceWithText ("a different recording altogether"));
    fixture.library.upsertTrack (fixture.recordFor (fixture.first, "Yeah", "Boys Noize"));
    fixture.library.store (fixture.first, analysisAt (130.0), 300.0);

    const auto known = fixture.library.lookup (fixture.first);
    REQUIRE (known.has_value());
    REQUIRE_THAT (known->bpm, WithinAbs (130.0, 1.0e-9));
}
