/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
*/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "library/TagReader.h"

#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
    using Bytes = std::vector<unsigned char>;

    void appendSyncsafe (Bytes& out, int value)
    {
        out.push_back ((unsigned char) ((value >> 21) & 0x7F));
        out.push_back ((unsigned char) ((value >> 14) & 0x7F));
        out.push_back ((unsigned char) ((value >> 7) & 0x7F));
        out.push_back ((unsigned char) (value & 0x7F));
    }

    void appendBigEndian (Bytes& out, int value)
    {
        out.push_back ((unsigned char) ((value >> 24) & 0xFF));
        out.push_back ((unsigned char) ((value >> 16) & 0xFF));
        out.push_back ((unsigned char) ((value >> 8) & 0xFF));
        out.push_back ((unsigned char) (value & 0xFF));
    }

    Bytes textFrame (const char* id, const Bytes& payload, int version)
    {
        Bytes frame (id, id + 4);

        if (version == 4)
            appendSyncsafe (frame, (int) payload.size());
        else
            appendBigEndian (frame, (int) payload.size());

        frame.push_back (0);
        frame.push_back (0);
        frame.insert (frame.end(), payload.begin(), payload.end());
        return frame;
    }

    Bytes latin1 (const juce::String& text)
    {
        Bytes out { 0 };

        for (auto c : text)
            out.push_back ((unsigned char) c);

        return out;
    }

    Bytes utf16WithBom (const juce::String& text)
    {
        Bytes out { 1, 0xFF, 0xFE };   // little endian

        for (auto c : text)
        {
            out.push_back ((unsigned char) (c & 0xFF));
            out.push_back ((unsigned char) ((c >> 8) & 0xFF));
        }

        return out;
    }

    Bytes utf8 (const juce::String& text)
    {
        Bytes out { 3 };
        const auto* raw = text.toRawUTF8();
        out.insert (out.end(), raw, raw + text.getNumBytesAsUTF8());
        return out;
    }

    Bytes tag (int version, const std::vector<Bytes>& frames, unsigned char flags = 0)
    {
        Bytes body;

        for (const auto& frame : frames)
            body.insert (body.end(), frame.begin(), frame.end());

        body.resize (body.size() + 16, 0);   // padding, as writers leave

        Bytes out { 'I', 'D', '3', (unsigned char) version, 0, flags };
        appendSyncsafe (out, (int) body.size());
        out.insert (out.end(), body.begin(), body.end());
        return out;
    }
}

TEST_CASE ("an ID3v2.3 tag yields title, artist and tempo", "[tags][id3]")
{
    const auto bytes = tag (3, { textFrame ("TIT2", latin1 ("Spastik"), 3),
                                 textFrame ("TPE1", utf16WithBom ("Plastikman"), 3),
                                 textFrame ("TALB", latin1 ("Sheet One"), 3),
                                 textFrame ("TBPM", latin1 ("134"), 3),
                                 textFrame ("TCON", latin1 ("(18)Techno"), 3),
                                 textFrame ("TKEY", latin1 ("Am"), 3) });

    opendj::TrackTags tags;
    const auto length = opendj::TagReader::parseId3v2 (bytes.data(), (int) bytes.size(), tags);

    REQUIRE (length == (int) bytes.size());
    REQUIRE (tags.title == "Spastik");
    REQUIRE (tags.artist == "Plastikman");
    REQUIRE (tags.album == "Sheet One");
    REQUIRE (tags.genre == "Techno");
    REQUIRE (tags.key == "Am");
    REQUIRE_THAT (tags.bpm, WithinAbs (134.0, 0.001));
}

TEST_CASE ("an ID3v2.4 tag with UTF-8 and syncsafe frame sizes is read", "[tags][id3]")
{
    const auto bytes = tag (4, { textFrame ("TIT2", utf8 (juce::CharPointer_UTF8 ("Bl\xc3\xa5t\xc3\xa5g")), 4),
                                 textFrame ("TPE1", utf8 ("Kraftwerk"), 4) });

    opendj::TrackTags tags;
    REQUIRE (opendj::TagReader::parseId3v2 (bytes.data(), (int) bytes.size(), tags) > 0);
    REQUIRE (tags.title == juce::String (juce::CharPointer_UTF8 ("Bl\xc3\xa5t\xc3\xa5g")));
    REQUIRE (tags.artist == "Kraftwerk");
}

TEST_CASE ("a block that is not a tag is refused", "[tags][id3]")
{
    const Bytes junk { 0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
    opendj::TrackTags tags;
    REQUIRE (opendj::TagReader::parseId3v2 (junk.data(), (int) junk.size(), tags) == 0);
    REQUIRE (tags.title.isEmpty());
}

TEST_CASE ("an ID3v1 block fills in what is missing", "[tags][id3]")
{
    Bytes block (128, 0);
    block[0] = 'T'; block[1] = 'A'; block[2] = 'G';
    const char* title = "Yeah";
    const char* artist = "Boys Noize";
    std::copy (title, title + 4, block.begin() + 3);
    std::copy (artist, artist + 10, block.begin() + 33);

    opendj::TrackTags tags;
    tags.title = "Already there";

    REQUIRE (opendj::TagReader::parseId3v1 (block.data(), 128, tags));
    REQUIRE (tags.title == "Already there");
    REQUIRE (tags.artist == "Boys Noize");
}

TEST_CASE ("an MP3 length comes from the Xing frame count when there is one", "[tags][mp3]")
{
    // MPEG 1 layer III, 128 kbps, 44100 Hz, joint stereo: FF FB 90 00.
    Bytes frame { 0xFF, 0xFB, 0x90, 0x00 };
    frame.resize (4 + 32, 0);                          // side information for stereo
    frame.insert (frame.end(), { 'X', 'i', 'n', 'g' });
    appendBigEndian (frame, 0x03);                     // frames and bytes present
    appendBigEndian (frame, 10000);                    // frames
    appendBigEndian (frame, 4000000);                  // bytes
    frame.resize (frame.size() + 64, 0);

    double sampleRate = 0.0;
    const auto seconds = opendj::TagReader::estimateMp3Duration (frame.data(), (int) frame.size(),
                                                                 4000000, &sampleRate);

    REQUIRE_THAT (seconds, WithinAbs (10000 * 1152.0 / 44100.0, 0.01));
    REQUIRE_THAT (sampleRate, WithinAbs (44100.0, 0.001));
}

TEST_CASE ("a constant bitrate MP3 length comes from the file size", "[tags][mp3]")
{
    Bytes frame { 0x00, 0x00, 0xFF, 0xFB, 0x90, 0x00 };   // a little junk, then the header
    frame.resize (frame.size() + 64, 0);

    const auto seconds = opendj::TagReader::estimateMp3Duration (frame.data(), (int) frame.size(), 1600000);
    REQUIRE_THAT (seconds, WithinAbs (1600000 * 8.0 / 128000.0, 0.01));

    const Bytes nothing (64, 0);
    REQUIRE_THAT (opendj::TagReader::estimateMp3Duration (nothing.data(), 64, 1600000), WithinAbs (0.0, 0.001));
}

TEST_CASE ("a file name is split into artist and title when the tags are empty", "[tags]")
{
    opendj::TrackTags tags;
    opendj::TagReader::guessFromFileName (juce::File ("/music/Boys Noize - Yeah.mp3"), tags);
    REQUIRE (tags.artist == "Boys Noize");
    REQUIRE (tags.title == "Yeah");

    tags = {};
    opendj::TagReader::guessFromFileName (juce::File ("/music/03 - Plastikman - Spastik.mp3"), tags);
    REQUIRE (tags.artist == "Plastikman");
    REQUIRE (tags.title == "Spastik");

    tags = {};
    opendj::TagReader::guessFromFileName (juce::File ("/music/untitled.wav"), tags);
    REQUIRE (tags.artist.isEmpty());
    REQUIRE (tags.title == "untitled");

    tags = {};
    tags.title = "Tagged";
    opendj::TagReader::guessFromFileName (juce::File ("/music/Someone - Something.mp3"), tags);
    REQUIRE (tags.title == "Tagged");   // tags win over the file name
}

TEST_CASE ("a WAV file's length and INFO chunk are read", "[tags][wav]")
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    juce::TemporaryFile temporary (".wav");
    {
        juce::AudioBuffer<float> silence (2, 44100 * 2);
        silence.clear();

        juce::StringPairArray metadata;
        metadata.set ("INAM", "Spastik");
        metadata.set ("IART", "Plastikman");

        juce::WavAudioFormat wav;
        auto stream = std::make_unique<juce::FileOutputStream> (temporary.getFile());
        REQUIRE (stream->openedOk());
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.release(), 44100.0, 2, 16, metadata, 0));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples()));
    }

    const auto tags = opendj::TagReader::read (formatManager, temporary.getFile());
    REQUIRE_THAT (tags.durationSeconds, WithinAbs (2.0, 0.001));
    REQUIRE_THAT (tags.sampleRate, WithinAbs (44100.0, 0.001));
    REQUIRE (tags.title == "Spastik");
    REQUIRE (tags.artist == "Plastikman");
}
