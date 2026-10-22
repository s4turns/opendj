/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "library/Library.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace opendj
{

/** One track as rekordbox describes it. */
struct RekordboxTrack
{
    juce::File file;

    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String genre;
    juce::String key;           ///< "Tonality", in whichever notation rekordbox was set to
    juce::String comment;

    double durationSeconds = 0.0;
    int rating = 0;             ///< 0 to 5 stars
    juce::uint32 colour = 0;    ///< ARGB, 0 for none

    /** Only a grid with one tempo throughout. OpenDJ's grid has a single
        tempo, and forcing a variable one onto it would put most beats in the
        wrong place, so such a track is left to the analyser instead. */
    bool hasGrid = false;
    double bpm = 0.0;
    double firstBeatSeconds = 0.0;   ///< a downbeat, as the analyser's is

    HotCues hotCues = noHotCues();
    HotCueNames hotCueNames = noHotCueNames();
};

struct RekordboxPlaylist
{
    juce::String name;               ///< folders and all: "Techno / Peak time"
    std::vector<juce::File> files;   ///< in order, whether or not they exist here
};

struct RekordboxCollection
{
    std::vector<RekordboxTrack> tracks;
    std::vector<RekordboxPlaylist> playlists;
    juce::String error;              ///< set when the file was not a rekordbox library
};

/** What an import did, for telling the user. */
struct RekordboxImportSummary
{
    int tracksImported = 0;
    int tracksMissing = 0;           ///< in the XML, but not at that path on this machine
    int gridsImported = 0;
    int hotCuesImported = 0;
    int playlistsImported = 0;
    juce::StringArray playlistsSkipped;   ///< names already taken in OpenDJ

    juce::String describe() const;
};

/** Reads the library rekordbox exports with File > Export Collection in xml
    format, and brings it into OpenDJ's.

    The XML is the one route into rekordbox that is documented and stable; the
    database it keeps for itself is encrypted. It carries tags, ratings,
    colours, comments, beat grids, hot cues and playlists, which is almost
    everything a DJ has put into a collection.

    Hot cues A to H land on pads 1 to 8. Memory cues have no counterpart here
    and are left out; a hot loop comes in as a cue at its start.
*/
class RekordboxImport
{
public:
    static RekordboxCollection parse (const juce::String& xml);
    static RekordboxCollection read (const juce::File& xmlFile);

    /** A file from a Location attribute: a file://localhost/ URL with its
        path percent-encoded, as UTF-8. */
    static juce::File fileFromLocation (const juce::String& location);

    /** Adds the tracks that exist on this machine and the playlists, and
        returns what it did. A playlist whose name is taken is left alone
        rather than overwritten, since it may be one the user built here. */
    static RekordboxImportSummary importInto (Library& library, const RekordboxCollection& collection);
};

} // namespace opendj
