/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "library/Library.h"

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
