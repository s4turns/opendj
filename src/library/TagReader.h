/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace opendj
{

/** What a file says about itself, before anyone listens to it. */
struct TrackTags
{
    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String genre;
    juce::String key;             ///< as written in the file, e.g. "Am" or "8A"
    double bpm = 0.0;             ///< the tagged tempo, which is a claim, not a measurement
    double durationSeconds = 0.0;
    double sampleRate = 0.0;
};

/** Reads titles, artists and lengths without decoding any audio.

    The scanner meets every file in a collection before any of them is played,
    so this has to be quick. For MP3 that means reading the ID3 tag and the
    first frame header by hand rather than opening a decoder, which would scan
    the whole file to find its length. Other formats keep their headers at the
    front and are cheap to open normally.
*/
class TagReader
{
public:
    static TrackTags read (juce::AudioFormatManager& formatManager, const juce::File& file);

    /** Parses an ID3v2 tag from the start of a block of bytes. Public so the
        parser can be tested without writing files. Returns the tag's total
        length in bytes, or 0 if the block does not begin with a tag. */
    static int parseId3v2 (const void* data, int numBytes, TrackTags& tags);

    /** Parses the 128 byte ID3v1 block from the end of a file. */
    static bool parseId3v1 (const void* data, int numBytes, TrackTags& tags);

    /** Estimates the length of an MP3 from its first frame header and, when
        present, its Xing/Info header. `audioBytes` is the file length less any
        tags. Returns 0 when no frame header is found. */
    static double estimateMp3Duration (const void* firstFrames, int numBytes, juce::int64 audioBytes,
                                       double* sampleRateOut = nullptr);

    /** "Artist - Title" from a file name, which is the convention most
        collections follow when the tags are missing. */
    static void guessFromFileName (const juce::File& file, TrackTags& tags);
};

} // namespace opendj
