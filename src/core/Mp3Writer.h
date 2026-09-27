/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <vector>

// LAME's public header is called lame.h and sits at the root of its include
// directory, which is too generic a name to put on every translation unit that
// wants an MP3 writer. Only Mp3Writer.cpp includes it; the handle is carried
// here as the incomplete type LAME itself names it with.
struct lame_global_struct;

namespace opendj
{

/** Writes MP3 through LAME, as an `AudioFormatWriter` so that everything else
    stays the same.

    That base class is the point. `SetRecorder` hands whatever writer it built
    to a `juce::AudioFormatWriter::ThreadedWriter`, which is where the FIFO, the
    background thread and the drop-rather-than-block bargain all live; a writer
    that fits the seam inherits all of it and the recorder needs to know nothing
    about MP3. `RtmpBroadcaster::PcmWriter` is the same trick around ffmpeg.

    Unlike JUCE's own `LAMEEncoderAudioFormat`, which is a wrapper around the
    lame command line program, this encodes block by block straight into the
    file. A three hour set does not become three gigabytes of temporary WAV
    waiting to be converted after the stop button, and nothing has to be
    installed for it to work.
*/
class Mp3Writer final : public juce::AudioFormatWriter
{
public:
    /** Takes the stream, whether or not the encoder starts: check
        `openedOk()` and throw the whole writer away if it says no.

        `bitrateKbps` is a constant bitrate in kilobits, or 0 for LAME's V0,
        which is variable and averages around 245. */
    Mp3Writer (std::unique_ptr<juce::OutputStream> streamToWriteTo,
               double sampleRate, int bitrateKbps);

    /** Flushes LAME's last frames and fixes up the Xing header. */
    ~Mp3Writer() override;

    bool openedOk() const noexcept { return lame != nullptr; }

    /** Called on the recording thread, never the audio thread. */
    bool write (const int** samplesToWrite, int numSamples) override;

    /** The rate LAME was told to write, which is not always the rate it was
        given: MP3 has no 88.2 or 96 kHz, and an audio device very much does,
        so anything MP3 cannot carry is resampled down to the nearest rate it
        can. Public for the test that covers exactly that. */
    static int nearestSupportedRate (double sampleRate);

private:
    bool encodeAndWrite (int numSamples);

    lame_global_struct* lame = nullptr;

    std::vector<short> left, right;
    std::vector<unsigned char> mp3Buffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Mp3Writer)
};

} // namespace opendj
