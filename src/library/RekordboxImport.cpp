/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "library/RekordboxImport.h"

#include <cmath>
#include <map>

namespace opendj
{

namespace
{
    /** Ratings are stored as 0, 51, 102, 153, 204 and 255. */
    int starsFromRating (int rating)
    {
        return juce::jlimit (0, 5, juce::roundToInt (rating / 51.0));
    }

    /** "0xFF007F", with no alpha. Absent means no colour. */
    juce::uint32 colourFromAttribute (const juce::String& text)
    {
        const auto hex = text.trim();

        if (! hex.startsWithIgnoreCase ("0x") || hex.length() < 3)
            return 0;

        return 0xff000000u | ((juce::uint32) hex.substring (2).getHexValue32() & 0x00ffffffu);
    }

    /** Reads the beat grid, if it has one tempo throughout. rekordbox writes
        a TEMPO element wherever the tempo changes; Inizio is where that beat
        falls and Battito which beat of its bar it is, 1 being the downbeat. */
    void readGrid (const juce::XmlElement& trackElement, RekordboxTrack& track)
    {
        const juce::XmlElement* first = nullptr;

        for (auto* tempo : trackElement.getChildWithTagNameIterator ("TEMPO"))
        {
            if (first == nullptr)
                first = tempo;
            else if (std::abs (tempo->getDoubleAttribute ("Bpm") - first->getDoubleAttribute ("Bpm")) > 0.005)
                return;
        }

        if (first == nullptr)
            return;

        const auto bpm = first->getDoubleAttribute ("Bpm");

        if (bpm <= 0.0)
            return;

        // Back from that beat to its bar's downbeat, then back by whole bars to
        // the first one in the track, so the bars line up as they did there.
        const auto beat = 60.0 / bpm;
        const auto bar = 4.0 * beat;
        const auto beatInBar = juce::jlimit (1, 4, first->getIntAttribute ("Battito", 1));
        auto downbeat = std::fmod (first->getDoubleAttribute ("Inizio") - (beatInBar - 1) * beat, bar);

        if (downbeat < 0.0)
            downbeat += bar;

        track.hasGrid = true;
        track.bpm = bpm;
        track.firstBeatSeconds = downbeat;
    }

    /** Hot cues are the marks with a Num of 0 and up; -1 is a memory cue. */
    void readHotCues (const juce::XmlElement& trackElement, RekordboxTrack& track)
    {
        for (auto* mark : trackElement.getChildWithTagNameIterator ("POSITION_MARK"))
        {
            const auto slot = mark->getIntAttribute ("Num", -1);
            const auto start = mark->getDoubleAttribute ("Start", -1.0);

            if (juce::isPositiveAndBelow (slot, numHotCues) && start >= 0.0)
            {
                track.hotCues[(size_t) slot] = start;
                track.hotCueNames[(size_t) slot] = mark->getStringAttribute ("Name");
            }
        }
    }

    RekordboxTrack readTrack (const juce::XmlElement& element)
    {
        RekordboxTrack track;
        track.file = RekordboxImport::fileFromLocation (element.getStringAttribute ("Location"));
        track.title = element.getStringAttribute ("Name");
        track.artist = element.getStringAttribute ("Artist");
        track.album = element.getStringAttribute ("Album");
        track.genre = element.getStringAttribute ("Genre");
        track.key = element.getStringAttribute ("Tonality");
        track.comment = element.getStringAttribute ("Comments");
        track.durationSeconds = element.getDoubleAttribute ("TotalTime");
        track.rating = starsFromRating (element.getIntAttribute ("Rating"));
        track.colour = colourFromAttribute (element.getStringAttribute ("Colour"));

        readGrid (element, track);
        readHotCues (element, track);
        return track;
    }

    /** Walks the playlist tree. Type 0 is a folder, 1 a playlist; KeyType 0
        means its entries name tracks by TrackID, 1 by Location. */
    void readPlaylists (const juce::XmlElement& node,
                        const juce::String& prefix,
                        const std::map<juce::String, juce::File>& filesById,
                        std::vector<RekordboxPlaylist>& playlists)
    {
        for (auto* child : node.getChildWithTagNameIterator ("NODE"))
        {
            const auto name = child->getStringAttribute ("Name").trim();
            const auto path = prefix.isEmpty() ? name : prefix + " / " + name;

            if (child->getIntAttribute ("Type") == 0)
            {
                readPlaylists (*child, path, filesById, playlists);
                continue;
            }

            RekordboxPlaylist playlist;
            playlist.name = path;

            const auto byLocation = child->getIntAttribute ("KeyType") == 1;

            for (auto* entry : child->getChildWithTagNameIterator ("TRACK"))
            {
                const auto key = entry->getStringAttribute ("Key");

                if (byLocation)
                {
                    if (const auto file = RekordboxImport::fileFromLocation (key); file != juce::File())
                        playlist.files.push_back (file);
                }
                else if (const auto found = filesById.find (key); found != filesById.end())
                {
                    playlist.files.push_back (found->second);
                }
            }

            if (playlist.name.isNotEmpty())
                playlists.push_back (std::move (playlist));
        }
    }

    juce::String plural (int count, const juce::String& noun)
    {
        return juce::String (count) + " " + noun + (count == 1 ? "" : "s");
    }
}

//==============================================================================

juce::File RekordboxImport::fileFromLocation (const juce::String& location)
{
    auto text = location.trim();

    if (text.startsWithIgnoreCase ("file://localhost/"))
        text = text.substring (16);
    else if (text.startsWithIgnoreCase ("file:///"))
        text = text.substring (7);
    else
        return {};

    // Percent-escapes are bytes of UTF-8, so the decoding happens on bytes.
    // juce::URL's own decoder also turns '+' into a space, which would break
    // any file with a plus in its name.
    juce::MemoryOutputStream bytes;
    const auto* utf8 = text.toRawUTF8();

    for (size_t i = 0; utf8[i] != 0; ++i)
    {
        const auto high = utf8[i] == '%' ? juce::CharacterFunctions::getHexDigitValue ((juce::juce_wchar) utf8[i + 1]) : -1;
        const auto low = high >= 0 ? juce::CharacterFunctions::getHexDigitValue ((juce::juce_wchar) utf8[i + 2]) : -1;

        if (low >= 0)
        {
            bytes.writeByte ((char) ((high << 4) | low));
            i += 2;
        }
        else
        {
            bytes.writeByte (utf8[i]);
        }
    }

    auto path = juce::String::fromUTF8 (static_cast<const char*> (bytes.getData()), (int) bytes.getDataSize());

    // A collection exported on Windows: "/C:/Music/..." is "C:/Music/...".
    if (path.length() >= 3 && path[0] == '/' && juce::CharacterFunctions::isLetter (path[1]) && path[2] == ':')
        path = path.substring (1);

    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

RekordboxCollection RekordboxImport::parse (const juce::String& xml)
{
    RekordboxCollection collection;
    const auto root = juce::XmlDocument::parse (xml);

    if (root == nullptr || ! root->hasTagName ("DJ_PLAYLISTS"))
    {
        collection.error = "This is not a rekordbox collection. In rekordbox, use File > Export Collection in xml format.";
        return collection;
    }

    std::map<juce::String, juce::File> filesById;

    if (auto* tracks = root->getChildByName ("COLLECTION"))
    {
        for (auto* element : tracks->getChildWithTagNameIterator ("TRACK"))
        {
            auto track = readTrack (*element);

            if (track.file == juce::File())
                continue;

            filesById[element->getStringAttribute ("TrackID")] = track.file;
            collection.tracks.push_back (std::move (track));
        }
    }

    if (auto* playlists = root->getChildByName ("PLAYLISTS"))
        if (auto* top = playlists->getChildByName ("NODE"))   // "ROOT", which is not shown in rekordbox
            readPlaylists (*top, {}, filesById, collection.playlists);

    return collection;
}

RekordboxCollection RekordboxImport::read (const juce::File& xmlFile)
{
    if (! xmlFile.existsAsFile())
    {
        RekordboxCollection collection;
        collection.error = "Could not find " + xmlFile.getFullPathName();
        return collection;
    }

    return parse (xmlFile.loadFileAsString());
}

RekordboxImportSummary RekordboxImport::importInto (Library& library, const RekordboxCollection& collection)
{
    RekordboxImportSummary summary;

    for (const auto& track : collection.tracks)
    {
        if (! track.file.existsAsFile())
        {
            ++summary.tracksMissing;
            continue;
        }

        // What rekordbox knows wins over what is here, field by field, but an
        // empty field there does not wipe one that is filled in here.
        auto record = library.findTrack (track.file).value_or (TrackRecord());
        const auto prefer = [] (juce::String& field, const juce::String& value)
        {
            if (value.isNotEmpty())
                field = value;
        };

        record.file = track.file;
        prefer (record.title, track.title);
        prefer (record.artist, track.artist);
        prefer (record.album, track.album);
        prefer (record.genre, track.genre);
        prefer (record.key, track.key);
        record.fileSize = track.file.getSize();
        record.fileModified = track.file.getLastModificationTime().toMilliseconds();

        if (track.durationSeconds > 0.0)
            record.durationSeconds = track.durationSeconds;

        // A grid someone checked by ear in rekordbox is trusted over a
        // detected one. Without one, the analysis here stays if the file is
        // unchanged and is redone if it is not.
        record.analysed = track.hasGrid;

        if (track.hasGrid)
        {
            record.bpm = track.bpm;
            record.firstBeatSeconds = track.firstBeatSeconds;
            record.tempoConfidence = 1.0f;
            ++summary.gridsImported;
        }

        if (library.upsertTrack (record) == 0)
            continue;

        library.setTrackDetails (track.file,
                                 track.rating > 0 ? track.rating : record.rating,
                                 track.colour != 0 ? track.colour : record.colour,
                                 track.comment.isNotEmpty() ? track.comment : record.comment);

        // rekordbox's cues take their pads; pads it leaves empty keep theirs.
        auto cues = library.getHotCues (track.file);
        auto changed = false;

        for (int slot = 0; slot < numHotCues; ++slot)
        {
            if (track.hotCues[(size_t) slot] >= 0.0)
            {
                cues[(size_t) slot] = track.hotCues[(size_t) slot];
                changed = true;
                ++summary.hotCuesImported;
            }
        }

        if (changed)
        {
            library.setHotCues (track.file, cues);

            // Names go in after the positions, since they are kept on the same
            // row. rekordbox leaves plenty of cues unnamed, so only set the ones
            // that carry a name.
            for (int slot = 0; slot < numHotCues; ++slot)
                if (track.hotCues[(size_t) slot] >= 0.0 && track.hotCueNames[(size_t) slot].isNotEmpty())
                    library.setHotCueName (track.file, slot, track.hotCues[(size_t) slot],
                                           track.hotCueNames[(size_t) slot]);
        }

        ++summary.tracksImported;
    }

    juce::StringArray taken;

    for (const auto& existing : library.getPlaylists())
        taken.add (existing.name);

    for (const auto& playlist : collection.playlists)
    {
        if (taken.contains (playlist.name, true))
        {
            summary.playlistsSkipped.add (playlist.name);
            continue;
        }

        const auto id = library.createPlaylist (playlist.name);

        if (id == 0)
        {
            summary.playlistsSkipped.add (playlist.name);
            continue;
        }

        library.addToPlaylist (id, playlist.files);
        taken.add (playlist.name);
        ++summary.playlistsImported;
    }

    return summary;
}

juce::String RekordboxImportSummary::describe() const
{
    juce::String text;
    text << "Imported " << plural (tracksImported, "track") << ", with "
         << plural (gridsImported, "beat grid") << " and " << plural (hotCuesImported, "hot cue")
         << ", and " << plural (playlistsImported, "playlist") << ".";

    if (tracksMissing > 0)
        text << "\n\n" << plural (tracksMissing, "track") << (tracksMissing == 1 ? " is" : " are")
             << " not on this computer at the path rekordbox has, so "
             << (tracksMissing == 1 ? "it was" : "they were") << " left out.";

    if (! playlistsSkipped.isEmpty())
        text << "\n\nLeft alone, because OpenDJ already has a playlist by that name: "
             << playlistsSkipped.joinIntoString (", ") << ".";

    return text;
}

} // namespace opendj
