/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/Mp3Writer.h"

#include <lame.h>

#include <cmath>
#include <cstdarg>
#include <iterator>
#include <limits>

namespace opendj
{

namespace
{
    // MPEG-1 and MPEG-2 layer III between them carry these and nothing else.
    constexpr int supportedRates[] = { 8000, 11025, 12000, 16000, 22050,
                                       24000, 32000, 44100, 48000 };

    /** LAME's own guarantee: this many bytes can always take the frames one
        call produces. Anything smaller and it refuses the block. */
    size_t bufferBytesFor (int numSamples)
    {
        return (size_t) (1.25 * numSamples) + 7200;
    }

    /** LAME reports through these, and by default they go to stderr. A DJ
        application has nowhere useful to put that, and a per-frame warning
        would be thousands of lines over a set. */
    void silence (const char*, va_list) {}
}

int Mp3Writer::nearestSupportedRate (double sampleRate)
{
    // A whole-number ratio first, highest such rate wins. 88.2 kHz is a
    // doubled 44.1 and 96 a doubled 48, and halving a rate is a clean
    // decimation where a fractional resample is an interpolation with a filter
    // behind it. Nearest in Hz would send 88.2 to 48, which is both the wrong
    // family and the harder sum.
    const auto isWholeRatio = [sampleRate] (int rate)
    {
        const auto ratio = sampleRate / (double) rate;
        return ratio >= 1.0 && std::abs (ratio - std::round (ratio)) < 1.0e-6;
    };

    for (auto it = std::rbegin (supportedRates); it != std::rend (supportedRates); ++it)
        if (isWholeRatio (*it))
            return *it;

    // Nothing divides it, so the closest one there is. A rate this far off the
    // usual grid is not something an audio device produces, but a settings
    // file or a test can ask for one.
    auto best = supportedRates[0];
    auto bestDistance = std::numeric_limits<double>::max();

    for (const auto rate : supportedRates)
    {
        if (const auto distance = std::abs (sampleRate - (double) rate); distance < bestDistance)
        {
            bestDistance = distance;
            best = rate;
        }
    }

    return best;
}

Mp3Writer::Mp3Writer (std::unique_ptr<juce::OutputStream> streamToWriteTo,
                      double rate, int bitrateKbps)
    : AudioFormatWriter (streamToWriteTo.release(), "MP3", rate, 2u, 16u)
{
    auto* flags = lame_init();

    if (flags == nullptr)
        return;

    lame_set_errorf (flags, silence);
    lame_set_debugf (flags, silence);
    lame_set_msgf (flags, silence);

    lame_set_in_samplerate (flags, (int) (rate + 0.5));
    lame_set_out_samplerate (flags, nearestSupportedRate (rate));
    lame_set_num_channels (flags, 2);
    lame_set_mode (flags, JOINT_STEREO);

    // 2 is LAME's own "near best quality, not too slow". The recording thread
    // has a two second FIFO behind it and a set to get through, so the couple
    // of percent that 0 would add is not worth the risk of falling behind.
    lame_set_quality (flags, 2);

    if (bitrateKbps > 0)
    {
        lame_set_VBR (flags, vbr_off);
        lame_set_brate (flags, bitrateKbps);
    }
    else
    {
        lame_set_VBR (flags, vbr_mtrh);
        lame_set_VBR_q (flags, 0);
    }

    // The Xing header, which is what lets a player seek a variable bitrate
    // file and report its length. Turning it on here reserves an empty frame
    // at the front of the stream; the destructor fills it in.
    lame_set_bWriteVbrTag (flags, 1);

    if (lame_init_params (flags) < 0)
    {
        lame_close (flags);
        return;
    }

    lame = flags;
}

Mp3Writer::~Mp3Writer()
{
    if (lame == nullptr)
        return;

    mp3Buffer.resize (bufferBytesFor (0));

    if (const auto bytes = lame_encode_flush (lame, mp3Buffer.data(), (int) mp3Buffer.size());
        bytes > 0)
    {
        output->write (mp3Buffer.data(), (size_t) bytes);
    }

    // The real Xing header goes over the placeholder at the front, and can
    // only be written now: until the flush above there was no knowing how long
    // the file is or what its frames weigh. A file left with the placeholder
    // still plays, but reports the wrong length and cannot be scrubbed, which
    // for a two hour set is most of the value of having it.
    //
    // Asked for its size first, which is LAME's documented way round this: a
    // buffer that is too small is answered with the size it wanted rather than
    // by writing a partial frame.
    if (const auto needed = lame_get_lametag_frame (lame, nullptr, 0); needed > 0)
    {
        std::vector<unsigned char> tag (needed);

        if (lame_get_lametag_frame (lame, tag.data(), tag.size()) == needed)
        {
            // A stream that will not wind back keeps the placeholder. That is
            // a recording with a wrong duration, which is worth far less than
            // no recording at all, so it is not treated as a failure.
            output->flush();

            if (output->setPosition (0))
                output->write (tag.data(), tag.size());
        }
    }

    lame_close (lame);
    lame = nullptr;
}

bool Mp3Writer::write (const int** samplesToWrite, int numSamples)
{
    if (lame == nullptr)
        return false;

    if (numSamples <= 0)
        return true;

    left.resize ((size_t) numSamples);
    right.resize ((size_t) numSamples);

    // Samples arrive scaled to the full 32-bit range whatever bit depth this
    // writer declares, which is `AudioFormatWriter::write`'s own contract. The
    // top 16 bits of each is the sample LAME wants, so this is a shift and not
    // a rescale -- the same conversion, for the same reason, as
    // `RtmpBroadcaster::PcmWriter`.
    const auto* l = samplesToWrite[0];
    const auto* r = samplesToWrite[1] != nullptr ? samplesToWrite[1] : l;

    for (int i = 0; i < numSamples; ++i)
    {
        left[(size_t) i]  = (short) (l[i] >> 16);
        right[(size_t) i] = (short) (r[i] >> 16);
    }

    return encodeAndWrite (numSamples);
}

bool Mp3Writer::encodeAndWrite (int numSamples)
{
    mp3Buffer.resize (bufferBytesFor (numSamples));

    const auto bytes = lame_encode_buffer (lame, left.data(), right.data(), numSamples,
                                           mp3Buffer.data(), (int) mp3Buffer.size());

    if (bytes < 0)
        return false;

    // Zero is ordinary: LAME holds samples back until it has a whole frame.
    if (bytes == 0)
        return true;

    return output->write (mp3Buffer.data(), (size_t) bytes);
}

} // namespace opendj
