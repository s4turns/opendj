/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "library/RekordboxImport.h"

using Catch::Matchers::WithinAbs;

namespace
{
    /** A Location as rekordbox writes it: every byte outside a few safe
        characters percent-encoded, except '/' and, as rekordbox does, '+'. */
    juce::String locationOf (const juce::File& file)
    {
        juce::String location ("file://localhost");
        const auto* utf8 = file.getFullPathName().toRawUTF8();

        for (size_t i = 0; utf8[i] != 0; ++i)
        {
            const auto c = (unsigned char) utf8[i];

            if (juce::CharacterFunctions::isLetterOrDigit ((juce::juce_wchar) c) && c < 0x80)
                location << (char) c;
            else if (c == '/' || c == '-' || c == '.' || c == '_' || c == '+')
                location << (char) c;
            else
                location << "%" << juce::String::toHexString ((int) c).toUpperCase().paddedLeft ('0', 2);
        }

        return location;
    }

    /** Two real files, one that is not there, and a library to import into. */
    struct Fixture
    {
        Fixture()
        {
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("opendj-rekordbox-test-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            REQUIRE (folder.createDirectory());
            REQUIRE (library.open (folder.getChildFile ("library.sqlite")).wasOk());

            yeah = folder.getChildFile ("Boys Noize - Yeah.mp3");
            snow = folder.getChildFile ("My Music").getChildFile (juce::String::fromUTF8 ("\xc3\x85ke + Friends - Sn\xc3\xb6.wav"));
            missing = folder.getChildFile ("Elsewhere - Gone.mp3");

            snow.getParentDirectory().createDirectory();
            REQUIRE (yeah.replaceWithText ("not really audio"));
            REQUIRE (snow.replaceWithText ("nor this"));
        }

        ~Fixture()
        {
            library.close();
            folder.deleteRecursively();
        }

        juce::String xml() const
        {
            return R"(<?xml version="1.0" encoding="UTF-8"?>
<DJ_PLAYLISTS Version="1.0.0">
  <PRODUCT Name="rekordbox" Version="6.8.5" Company="AlphaTheta"/>
  <COLLECTION Entries="3">
    <TRACK TrackID="11" Name="Yeah" Artist="Boys Noize" Album="Oi Oi Oi" Genre="Electro" Kind="MP3 File"
           TotalTime="312" AverageBpm="128.00" Rating="204" Colour="0xFF007F" Comments="peak time"
           Tonality="8A" Location=")" + locationOf (yeah) + R"(">
      <TEMPO Inizio="1.250" Bpm="128.00" Metro="4/4" Battito="3"/>
      <TEMPO Inizio="61.250" Bpm="128.00" Metro="4/4" Battito="1"/>
      <POSITION_MARK Name="drop" Type="0" Start="64.012" Num="0" Red="40" Green="226" Blue="20"/>
      <POSITION_MARK Name="" Type="4" Start="96.000" End="103.500" Num="3"/>
      <POSITION_MARK Name="memory" Type="0" Start="10.000" Num="-1"/>
      <POSITION_MARK Name="beyond" Type="0" Start="20.000" Num="8"/>
    </TRACK>
    <TRACK TrackID="12" Name="Sn&#246;" Artist="&#197;ke + Friends" TotalTime="200" Rating="0"
           Location=")" + locationOf (snow) + R"(">
      <TEMPO Inizio="0.100" Bpm="120.00" Metro="4/4" Battito="1"/>
      <TEMPO Inizio="30.100" Bpm="124.00" Metro="4/4" Battito="1"/>
    </TRACK>
    <TRACK TrackID="13" Name="Gone" Artist="Elsewhere" Location=")" + locationOf (missing) + R"("/>
  </COLLECTION>
  <PLAYLISTS>
    <NODE Type="0" Name="ROOT" Count="2">
      <NODE Type="0" Name="Techno" Count="1">
        <NODE Type="1" Name="Peak time" KeyType="0" Entries="3">
          <TRACK Key="12"/>
          <TRACK Key="11"/>
          <TRACK Key="13"/>
        </NODE>
      </NODE>
      <NODE Type="1" Name="Warm up" KeyType="1" Entries="1">
        <TRACK Key=")" + locationOf (yeah) + R"("/>
      </NODE>
    </NODE>
  </PLAYLISTS>
</DJ_PLAYLISTS>)";
        }

        opendj::Library library;
        juce::File folder, yeah, snow, missing;
    };
}

TEST_CASE ("a rekordbox location becomes the file it names", "[library][rekordbox]")
{
    using opendj::RekordboxImport;

    REQUIRE (RekordboxImport::fileFromLocation ("file://localhost/Users/dj/My%20Music/a.mp3")
             == juce::File ("/Users/dj/My Music/a.mp3"));

    // UTF-8 bytes, and a plus that stays a plus.
    REQUIRE (RekordboxImport::fileFromLocation ("file://localhost/Music/%C3%85ke%20+%20Friends.wav")
             == juce::File (juce::String::fromUTF8 ("/Music/\xc3\x85ke + Friends.wav")));

    REQUIRE (RekordboxImport::fileFromLocation ("file:///Music/a.mp3") == juce::File ("/Music/a.mp3"));

    // Not a file at all.
    REQUIRE (RekordboxImport::fileFromLocation ("http://example.com/a.mp3") == juce::File());
    REQUIRE (RekordboxImport::fileFromLocation ({}) == juce::File());

   #if JUCE_WINDOWS
    REQUIRE (RekordboxImport::fileFromLocation ("file://localhost/C:/Music/a.mp3") == juce::File ("C:\\Music\\a.mp3"));
   #endif
}

TEST_CASE ("a rekordbox collection is read into tracks and playlists", "[library][rekordbox]")
{
    Fixture fixture;
    const auto collection = opendj::RekordboxImport::parse (fixture.xml());

    REQUIRE (collection.error.isEmpty());
    REQUIRE (collection.tracks.size() == 3);

    const auto& yeah = collection.tracks[0];
    REQUIRE (yeah.file == fixture.yeah);
    REQUIRE (yeah.title == "Yeah");
    REQUIRE (yeah.artist == "Boys Noize");
    REQUIRE (yeah.album == "Oi Oi Oi");
    REQUIRE (yeah.genre == "Electro");
    REQUIRE (yeah.key == "8A");
    REQUIRE (yeah.comment == "peak time");
    REQUIRE (yeah.rating == 4);
    REQUIRE (yeah.colour == 0xffff007fu);
    REQUIRE_THAT (yeah.durationSeconds, WithinAbs (312.0, 1.0e-9));

    // The first TEMPO is beat 3 of its bar at 1.25 s: the downbeat is two
    // beats earlier, then whole bars back towards the start.
    REQUIRE (yeah.hasGrid);
    REQUIRE_THAT (yeah.bpm, WithinAbs (128.0, 1.0e-9));
    const auto beat = 60.0 / 128.0;
    const auto expected = std::fmod (1.25 - 2.0 * beat + 4.0 * beat, 4.0 * beat);
    REQUIRE_THAT (yeah.firstBeatSeconds, WithinAbs (expected, 1.0e-9));

    // Pads A and D; the memory cue and anything past pad 8 are not hot cues.
    REQUIRE_THAT (yeah.hotCues[0], WithinAbs (64.012, 1.0e-9));
    REQUIRE_THAT (yeah.hotCues[3], WithinAbs (96.0, 1.0e-9));

    // The mark's name comes across; an unnamed one stays empty.
    REQUIRE (yeah.hotCueNames[0] == "drop");
    REQUIRE (yeah.hotCueNames[3].isEmpty());

    for (const auto slot : { 1, 2, 4, 5, 6, 7 })
        REQUIRE (yeah.hotCues[(size_t) slot] < 0.0);

    const auto& snow = collection.tracks[1];
    REQUIRE (snow.file == fixture.snow);
    REQUIRE (snow.title == juce::String::fromUTF8 ("Sn\xc3\xb6"));
    REQUIRE (snow.rating == 0);
    REQUIRE (snow.colour == 0);
    REQUIRE_FALSE (snow.hasGrid);   // two tempos: left to the analyser

    REQUIRE (collection.playlists.size() == 2);
    REQUIRE (collection.playlists[0].name == "Techno / Peak time");
    REQUIRE (collection.playlists[0].files == std::vector<juce::File> { fixture.snow, fixture.yeah, fixture.missing });
    REQUIRE (collection.playlists[1].name == "Warm up");
    REQUIRE (collection.playlists[1].files == std::vector<juce::File> { fixture.yeah });
}

TEST_CASE ("something that is not a rekordbox collection says so", "[library][rekordbox]")
{
    REQUIRE (opendj::RekordboxImport::parse ("<plist version=\"1.0\"><dict/></plist>").error.isNotEmpty());
    REQUIRE (opendj::RekordboxImport::parse ("not xml at all").error.isNotEmpty());
    REQUIRE (opendj::RekordboxImport::read (juce::File ("/no/such/collection.xml")).error.isNotEmpty());
}

TEST_CASE ("importing a rekordbox collection fills the library", "[library][rekordbox]")
{
    Fixture fixture;
    const auto summary = opendj::RekordboxImport::importInto (fixture.library,
                                                              opendj::RekordboxImport::parse (fixture.xml()));

    REQUIRE (summary.tracksImported == 2);
    REQUIRE (summary.tracksMissing == 1);
    REQUIRE (summary.gridsImported == 1);
    REQUIRE (summary.hotCuesImported == 2);
    REQUIRE (summary.playlistsImported == 2);
    REQUIRE (summary.playlistsSkipped.isEmpty());
    REQUIRE (summary.describe().contains ("1 track is not on this computer"));

    const auto yeah = fixture.library.findTrack (fixture.yeah);
    REQUIRE (yeah.has_value());
    REQUIRE (yeah->title == "Yeah");
    REQUIRE (yeah->key == "8A");
    REQUIRE (yeah->rating == 4);
    REQUIRE (yeah->colour == 0xffff007fu);
    REQUIRE (yeah->comment == "peak time");
    REQUIRE (yeah->analysed);
    REQUIRE_THAT (yeah->bpm, WithinAbs (128.0, 1.0e-9));

    // The deck gets the grid and the cues without analysing anything.
    const auto known = fixture.library.lookup (fixture.yeah);
    REQUIRE (known.has_value());
    REQUIRE (known->analysed);
    REQUIRE_THAT (known->hotCues[0], WithinAbs (64.012, 1.0e-9));
    REQUIRE (fixture.library.getHotCueNames (fixture.yeah)[0] == "drop");

    // No grid to take, so this one waits for the analyser.
    const auto snow = fixture.library.findTrack (fixture.snow);
    REQUIRE (snow.has_value());
    REQUIRE_FALSE (snow->analysed);
    REQUIRE (fixture.library.tracksNeedingAnalysis (10) == std::vector<juce::File> { fixture.snow });

    REQUIRE_FALSE (fixture.library.findTrack (fixture.missing).has_value());

    // A playlist keeps the missing track's entry, as playlists here do for a
    // disk that is not plugged in.
    const auto playlists = fixture.library.getPlaylists();
    REQUIRE (playlists.size() == 2);
    REQUIRE (playlists[0].name == "Techno / Peak time");
    REQUIRE (fixture.library.getPlaylistFiles (playlists[0].id)
             == std::vector<juce::File> { fixture.snow, fixture.yeah, fixture.missing });
}

TEST_CASE ("importing again leaves what was made here alone", "[library][rekordbox]")
{
    Fixture fixture;

    // Things done in OpenDJ before the import: a playlist with a name rekordbox
    // also uses, a cue on a pad rekordbox leaves empty, and a comment.
    const auto mine = fixture.library.createPlaylist ("Warm up");
    REQUIRE (fixture.library.addToPlaylist (mine, { fixture.snow }));

    opendj::TrackRecord record;
    record.file = fixture.yeah;
    record.title = "Yeah";
    record.fileSize = fixture.yeah.getSize();
    record.fileModified = fixture.yeah.getLastModificationTime().toMilliseconds();
    REQUIRE (fixture.library.upsertTrack (record) != 0);

    auto cues = opendj::noHotCues();
    cues[0] = 1.0;   // rekordbox has pad A too, and wins it
    cues[6] = 150.0;
    REQUIRE (fixture.library.setHotCues (fixture.yeah, cues));
    REQUIRE (fixture.library.setTrackDetails (fixture.snow, 0, 0, {}) == false);   // not in the library yet

    const auto collection = opendj::RekordboxImport::parse (fixture.xml());
    auto summary = opendj::RekordboxImport::importInto (fixture.library, collection);

    REQUIRE (summary.playlistsImported == 1);
    REQUIRE (summary.playlistsSkipped == juce::StringArray { "Warm up" });
    REQUIRE (fixture.library.getPlaylistFiles (mine) == std::vector<juce::File> { fixture.snow });

    const auto merged = fixture.library.getHotCues (fixture.yeah);
    REQUIRE_THAT (merged[0], WithinAbs (64.012, 1.0e-9));
    REQUIRE_THAT (merged[3], WithinAbs (96.0, 1.0e-9));
    REQUIRE_THAT (merged[6], WithinAbs (150.0, 1.0e-9));

    // A comment written here is not wiped by rekordbox having none.
    REQUIRE (fixture.library.setTrackDetails (fixture.snow, 2, 0, "written here"));
    summary = opendj::RekordboxImport::importInto (fixture.library, collection);

    REQUIRE (summary.playlistsImported == 0);
    REQUIRE (summary.playlistsSkipped.size() == 2);
    REQUIRE (fixture.library.getPlaylists().size() == 2);
    REQUIRE (fixture.library.findTrack (fixture.snow)->comment == "written here");
    REQUIRE (fixture.library.findTrack (fixture.snow)->rating == 2);
}
