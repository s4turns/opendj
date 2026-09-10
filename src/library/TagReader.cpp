/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "library/TagReader.h"

#include <cstring>

namespace opendj
{

namespace
{
    using Byte = unsigned char;

    int syncsafeToInt (const Byte* b) noexcept
    {
        return ((b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14) | ((b[2] & 0x7F) << 7) | (b[3] & 0x7F);
    }

    int bigEndianToInt (const Byte* b, int numBytes) noexcept
    {
        int value = 0;

        for (int i = 0; i < numBytes; ++i)
            value = (value << 8) | b[i];

        return value;
    }

    /** Undoes ID3's unsynchronisation, which inserts a zero after every 0xFF so
        that a tag can never look like an MP3 frame header. */
    juce::MemoryBlock removeUnsync (const Byte* data, int numBytes)
    {
        juce::MemoryBlock out;
        out.ensureSize ((size_t) numBytes);
        size_t written = 0;

        for (int i = 0; i < numBytes; ++i)
        {
            out[written++] = (char) data[i];

            if (data[i] == 0xFF && i + 1 < numBytes && data[i + 1] == 0x00)
                ++i;
        }

        out.setSize (written);
        return out;
    }

    /** Decodes the text of an ID3v2 text frame, whose first byte names the
        encoding. Several values in one frame are separated by nulls; they come
        back joined with a comma, which reads correctly for a list of artists. */
    juce::String decodeTextFrame (const Byte* data, int numBytes)
    {
        if (numBytes < 1)
            return {};

        const auto encoding = data[0];
        const auto* text = data + 1;
        const auto length = numBytes - 1;

        juce::String result;

        if (encoding == 0 || encoding == 3)
        {
            // Latin-1 or UTF-8: one byte per null.
            auto start = 0;

            for (int i = 0; i <= length; ++i)
            {
                if (i == length || text[i] == 0)
                {
                    if (i > start)
                    {
                        // Two pointers, not a count: the String constructor that
                        // takes a number counts characters, and a byte length
                        // handed to it reads past the end of the frame.
                        const auto decoded = encoding == 3
                            ? juce::String (juce::CharPointer_UTF8 ((const char*) text + start),
                                            juce::CharPointer_UTF8 ((const char*) text + i))
                            : [&]
                              {
                                  // Latin-1 above 0x7F is not ASCII, so build it
                                  // a character at a time rather than copying.
                                  juce::String latin;

                                  for (int k = start; k < i; ++k)
                                      latin << juce::String::charToString ((juce::juce_wchar) text[k]);

                                  return latin;
                              }();

                        if (result.isNotEmpty() && decoded.isNotEmpty())
                            result << ", ";

                        result << decoded;
                    }

                    start = i + 1;
                }
            }
        }
        else if (encoding == 1 || encoding == 2)
        {
            // UTF-16 with a byte order mark, or big endian without one.
            auto bigEndian = encoding == 2;
            auto offset = 0;

            if (encoding == 1 && length >= 2)
            {
                if (text[0] == 0xFF && text[1] == 0xFE)      { bigEndian = false; offset = 2; }
                else if (text[0] == 0xFE && text[1] == 0xFF) { bigEndian = true;  offset = 2; }
            }

            juce::String part;

            for (int i = offset; i + 1 < length; i += 2)
            {
                const auto unit = bigEndian ? (juce::uint16) ((text[i] << 8) | text[i + 1])
                                            : (juce::uint16) ((text[i + 1] << 8) | text[i]);

                if (unit == 0)
                {
                    if (part.isNotEmpty())
                    {
                        if (result.isNotEmpty())
                            result << ", ";

                        result << part;
                        part.clear();
                    }

                    // A new string may start with its own byte order mark.
                    if (encoding == 1 && i + 3 < length)
                    {
                        if (text[i + 2] == 0xFF && text[i + 3] == 0xFE)      { bigEndian = false; i += 2; }
                        else if (text[i + 2] == 0xFE && text[i + 3] == 0xFF) { bigEndian = true;  i += 2; }
                    }

                    continue;
                }

                if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < length)
                {
                    const auto low = bigEndian ? (juce::uint16) ((text[i + 2] << 8) | text[i + 3])
                                               : (juce::uint16) ((text[i + 3] << 8) | text[i + 2]);
                    const auto codepoint = 0x10000 + (((juce::uint32) unit - 0xD800) << 10) + ((juce::uint32) low - 0xDC00);
                    part << juce::String::charToString ((juce::juce_wchar) codepoint);
                    i += 2;
                }
                else
                {
                    part << juce::String::charToString ((juce::juce_wchar) unit);
                }
            }

            if (part.isNotEmpty())
            {
                if (result.isNotEmpty())
                    result << ", ";

                result << part;
            }
        }

        return result.trim();
    }

    void applyFrame (const juce::String& id, const juce::String& text, TrackTags& tags)
    {
        if (text.isEmpty())
            return;

        if (id == "TIT2" || id == "TT2")      tags.title = text;
        else if (id == "TPE1" || id == "TP1") tags.artist = text;
        else if (id == "TALB" || id == "TAL") tags.album = text;
        else if (id == "TKEY" || id == "TKE") tags.key = text;
        else if (id == "TBPM" || id == "TBP") tags.bpm = text.getDoubleValue();
        else if (id == "TCON" || id == "TCO")
        {
            // "(17)Rock" is the ID3v2.3 way of writing a genre; keep the words.
            auto genre = text;

            while (genre.startsWithChar ('(') && genre.containsChar (')'))
                genre = genre.fromFirstOccurrenceOf (")", false, false);

            tags.genre = genre.trim().isNotEmpty() ? genre.trim() : text;
        }
    }

    juce::String latin1Field (const Byte* data, int length)
    {
        juce::String s;

        for (int i = 0; i < length && data[i] != 0; ++i)
            s << juce::String::charToString ((juce::juce_wchar) data[i]);

        return s.trim();
    }

    // MPEG audio frame header tables, Layer III only, which is what "MP3" means.
    constexpr int bitrateTable[2][15] =
    {
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },   // unused: layer I
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 }        // MPEG 1 layer III
    };
    constexpr int bitrateTableMpeg2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    constexpr int sampleRateTable[3][3] =
    {
        { 44100, 48000, 32000 },   // MPEG 1
        { 22050, 24000, 16000 },   // MPEG 2
        { 11025, 12000, 8000 }     // MPEG 2.5
    };
}

//==============================================================================

int TagReader::parseId3v2 (const void* rawData, int numBytes, TrackTags& tags)
{
    const auto* data = static_cast<const Byte*> (rawData);

    if (numBytes < 10 || std::memcmp (data, "ID3", 3) != 0)
        return 0;

    const auto major = data[3];
    const auto flags = data[5];
    const auto tagSize = syncsafeToInt (data + 6);
    const auto totalSize = 10 + tagSize + ((flags & 0x10) ? 10 : 0);   // plus a footer in 2.4

    if (major < 2 || major > 4)
        return totalSize;

    const auto bodyLength = juce::jmin (tagSize, numBytes - 10);
    juce::MemoryBlock unsynced;
    const Byte* body = data + 10;

    if ((flags & 0x80) != 0 && major < 4)
    {
        unsynced = removeUnsync (body, bodyLength);
        body = static_cast<const Byte*> (unsynced.getData());
    }

    const auto bodySize = (flags & 0x80) != 0 && major < 4 ? (int) unsynced.getSize() : bodyLength;
    auto position = 0;

    if ((flags & 0x40) != 0 && major >= 3)
    {
        // Extended header. 2.4 counts its own size field and stores it
        // syncsafe; 2.3 does neither.
        if (bodySize < 4)
            return totalSize;

        position = major == 4 ? syncsafeToInt (body) : bigEndianToInt (body, 4) + 4;
    }

    const auto headerLength = major == 2 ? 6 : 10;

    while (position + headerLength <= bodySize)
    {
        if (body[position] == 0)
            break;   // padding

        juce::String id;
        int frameSize = 0;
        int frameFlags = 0;

        if (major == 2)
        {
            id = juce::String (juce::CharPointer_ASCII ((const char*) body + position), 3);
            frameSize = bigEndianToInt (body + position + 3, 3);
        }
        else
        {
            id = juce::String (juce::CharPointer_ASCII ((const char*) body + position), 4);
            frameSize = major == 4 ? syncsafeToInt (body + position + 4)
                                   : bigEndianToInt (body + position + 4, 4);
            frameFlags = bigEndianToInt (body + position + 8, 2);
        }

        position += headerLength;

        if (frameSize < 0 || position + frameSize > bodySize)
            break;

        if (id.startsWithChar ('T') && id != "TXXX" && id != "TXX")
        {
            const auto* frame = body + position;
            auto frameLength = frameSize;
            auto usable = true;
            juce::MemoryBlock frameUnsynced;

            if (major == 3)
            {
                if ((frameFlags & 0xC0) != 0)      // compressed or encrypted
                    usable = false;
                else if ((frameFlags & 0x20) != 0) // grouping identity byte
                    { frame += 1; frameLength -= 1; }
            }
            else if (major == 4)
            {
                if ((frameFlags & 0x0C) != 0)      // compressed or encrypted
                    usable = false;
                else
                {
                    if ((frameFlags & 0x40) != 0)  // grouping identity byte
                        { frame += 1; frameLength -= 1; }

                    if ((frameFlags & 0x01) != 0)  // data length indicator
                        { frame += 4; frameLength -= 4; }

                    if ((frameFlags & 0x02) != 0 && frameLength > 0)
                    {
                        frameUnsynced = removeUnsync (frame, frameLength);
                        frame = static_cast<const Byte*> (frameUnsynced.getData());
                        frameLength = (int) frameUnsynced.getSize();
                    }
                }
            }

            if (usable && frameLength > 0)
                applyFrame (id, decodeTextFrame (frame, frameLength), tags);
        }

        position += frameSize;
    }

    return totalSize;
}

bool TagReader::parseId3v1 (const void* rawData, int numBytes, TrackTags& tags)
{
    const auto* data = static_cast<const Byte*> (rawData);

    if (numBytes < 128 || std::memcmp (data, "TAG", 3) != 0)
        return false;

    if (tags.title.isEmpty())  tags.title = latin1Field (data + 3, 30);
    if (tags.artist.isEmpty()) tags.artist = latin1Field (data + 33, 30);
    if (tags.album.isEmpty())  tags.album = latin1Field (data + 63, 30);

    return true;
}

double TagReader::estimateMp3Duration (const void* rawData, int numBytes, juce::int64 audioBytes,
                                       double* sampleRateOut)
{
    const auto* data = static_cast<const Byte*> (rawData);

    for (int i = 0; i + 4 <= numBytes; ++i)
    {
        if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0)
            continue;

        const auto versionBits = (data[i + 1] >> 3) & 0x03;    // 0: 2.5, 2: 2, 3: 1
        const auto layerBits = (data[i + 1] >> 1) & 0x03;      // 1: layer III
        const auto bitrateIndex = (data[i + 2] >> 4) & 0x0F;
        const auto sampleRateIndex = (data[i + 2] >> 2) & 0x03;
        const auto channelMode = (data[i + 3] >> 6) & 0x03;

        if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3)
            continue;

        const auto mpeg1 = versionBits == 3;
        const auto versionRow = mpeg1 ? 0 : (versionBits == 2 ? 1 : 2);
        const auto sampleRate = sampleRateTable[versionRow][sampleRateIndex];
        const auto bitrateKbps = mpeg1 ? bitrateTable[1][bitrateIndex] : bitrateTableMpeg2[bitrateIndex];
        const auto samplesPerFrame = mpeg1 ? 1152 : 576;

        if (sampleRateOut != nullptr)
            *sampleRateOut = sampleRate;

        // A Xing or Info header sits after the side information and carries the
        // frame count, which is the only honest length for a variable bitrate file.
        const auto sideInfo = mpeg1 ? (channelMode == 3 ? 17 : 32) : (channelMode == 3 ? 9 : 17);
        const auto xing = i + 4 + sideInfo;

        if (xing + 12 <= numBytes
            && (std::memcmp (data + xing, "Xing", 4) == 0 || std::memcmp (data + xing, "Info", 4) == 0))
        {
            const auto xingFlags = bigEndianToInt (data + xing + 4, 4);

            if ((xingFlags & 0x01) != 0)
            {
                const auto frames = bigEndianToInt (data + xing + 8, 4);

                if (frames > 0)
                    return frames * (double) samplesPerFrame / sampleRate;
            }
        }

        if (bitrateKbps > 0 && audioBytes > 0)
            return (double) audioBytes * 8.0 / (bitrateKbps * 1000.0);

        return 0.0;
    }

    return 0.0;
}

void TagReader::guessFromFileName (const juce::File& file, TrackTags& tags)
{
    const auto name = file.getFileNameWithoutExtension().trim();

    if (tags.title.isNotEmpty())
        return;

    if (name.contains (" - "))
    {
        const auto artist = name.upToFirstOccurrenceOf (" - ", false, false).trim();
        const auto title = name.fromFirstOccurrenceOf (" - ", false, false).trim();

        // "01 - Artist - Title" has a track number in front; drop it.
        if (artist.containsOnly ("0123456789") && title.contains (" - "))
        {
            if (tags.artist.isEmpty())
                tags.artist = title.upToFirstOccurrenceOf (" - ", false, false).trim();

            tags.title = title.fromFirstOccurrenceOf (" - ", false, false).trim();
        }
        else
        {
            if (tags.artist.isEmpty())
                tags.artist = artist;

            tags.title = title;
        }
    }
    else
    {
        tags.title = name;
    }
}

TrackTags TagReader::read (juce::AudioFormatManager& formatManager, const juce::File& file)
{
    TrackTags tags;

    if (file.hasFileExtension ("mp3"))
    {
        juce::FileInputStream stream (file);

        if (stream.openedOk())
        {
            // Enough for the tag header, and then as much as the tag says it is.
            juce::MemoryBlock head;
            stream.readIntoMemoryBlock (head, 10);
            auto tagLength = 0;

            if (head.getSize() >= 10 && std::memcmp (head.getData(), "ID3", 3) == 0)
            {
                tagLength = 10 + syncsafeToInt (static_cast<const Byte*> (head.getData()) + 6);
                stream.setPosition (0);
                head.reset();
                // Tags with embedded artwork run to megabytes; there is no need to
                // read past the text frames, which come first by convention.
                stream.readIntoMemoryBlock (head, juce::jmin (tagLength, 1024 * 1024));
                parseId3v2 (head.getData(), (int) head.getSize(), tags);
            }

            juce::MemoryBlock frames;
            stream.setPosition (tagLength);
            stream.readIntoMemoryBlock (frames, 4096);

            auto audioBytes = file.getSize() - tagLength;
            juce::MemoryBlock tail;

            if (file.getSize() >= 128)
            {
                stream.setPosition (file.getSize() - 128);
                stream.readIntoMemoryBlock (tail, 128);

                if (parseId3v1 (tail.getData(), (int) tail.getSize(), tags))
                    audioBytes -= 128;
            }

            tags.durationSeconds = estimateMp3Duration (frames.getData(), (int) frames.getSize(),
                                                        audioBytes, &tags.sampleRate);
        }
    }
    else if (std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file)); reader != nullptr)
    {
        tags.sampleRate = reader->sampleRate;
        tags.durationSeconds = reader->sampleRate > 0.0
            ? (double) reader->lengthInSamples / reader->sampleRate
            : 0.0;

        // Each JUCE reader names its metadata after its own container.
        const auto& values = reader->metadataValues;
        const auto pick = [&values] (std::initializer_list<const char*> keys)
        {
            for (const auto* key : keys)
                if (const auto value = values[key].trim(); value.isNotEmpty())
                    return value;

            return juce::String();
        };

        tags.title = pick ({ "id3title", "INAM", "TITLE" });
        tags.artist = pick ({ "id3artist", "IART", "ARTIST" });
        tags.album = pick ({ "id3album", "IPRD", "ALBUM" });
        tags.genre = pick ({ "id3genre", "GENR", "GENRE" });
        tags.bpm = pick ({ "id3bpm", "BPM" }).getDoubleValue();
    }

    guessFromFileName (file, tags);
    return tags;
}

} // namespace opendj
