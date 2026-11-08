/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace opendj
{

/** A microphone, or a stereo line such as browser or system audio, laid over
    the mix the way the sampler is.

    The mic joins after the mixer and the sampler, so no fader or crossfader
    can take a voice away. Two things set it apart from anything else in the
    engine. It can be kept out of the speakers while still reaching the
    recording and the broadcasts, for somebody streaming from a room with the
    speakers in it. And it can duck the music while it is on, which is what
    the talkover button on a club mixer does.

    It is never on when the application starts, whatever was saved: an open
    mic in front of speakers howls, and nobody should get that without having
    reached for the button.
*/
class MicInput final
{
public:
    /** Where the voice is heard. */
    enum class Routing
    {
        everywhere,      ///< speakers, recording and broadcasts
        recordingOnly    ///< recording and broadcasts, never the speakers
    };

    /** How far talkover drops the music: to about a third of its level, enough
        for a voice to sit on top without the music stopping. */
    static constexpr float talkoverDecibels = -10.0f;

    /** How the chosen pair of inputs is heard. A mic is mono, so the pair is
        summed and the voice sits in the middle. A line input (the aux) keeps
        left and right apart. */
    enum class Source
    {
        mono,
        stereo
    };

    explicit MicInput (Source sourceType = Source::mono);

    Source getSource() const noexcept { return source; }

    /** Which pair of the device's active inputs this listens to: 0 is the
        first two, 1 the next two, and so on. A pair the device does not have
        is silent. Safe from any thread. */
    void setInputPair (int pairIndex) noexcept { inputPair.store (juce::jmax (0, pairIndex), std::memory_order_relaxed); }
    int getInputPair() const noexcept { return inputPair.load (std::memory_order_relaxed); }

    void prepare (double sampleRate);

    /** On or off, faded over a few milliseconds so neither clicks. Safe from
        any thread. */
    void setEnabled (bool shouldBeOn) noexcept { enabled.store (shouldBeOn, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    /** The mic's level as a gain: 1 is unity, 2 the most, about +6 dB. */
    void setGain (float newGain) noexcept;
    float getGain() const noexcept { return gain.load (std::memory_order_relaxed); }

    /** Whether the music ducks while the mic is on. */
    void setTalkover (bool shouldDuck) noexcept { talkover.store (shouldDuck, std::memory_order_relaxed); }
    bool isTalkoverEnabled() const noexcept { return talkover.load (std::memory_order_relaxed); }

    void setRouting (Routing newRouting) noexcept { routing.store (newRouting, std::memory_order_relaxed); }
    Routing getRouting() const noexcept { return routing.load (std::memory_order_relaxed); }

    /** The loudest the mic has been since the last call, after its level and
        whether or not it is on, so a level can be set before anybody hears it.
        For a meter. */
    float getAndResetPeak() noexcept { return peak.exchange (0.0f, std::memory_order_relaxed); }

    /** Ducks `room` for talkover and adds the voice to it. When the voice is
        kept out of the speakers, `room` gets only the ducking and `recording`
        is built as the mix to record and broadcast: the ducked music plus the
        voice.

        Returns true when `recording` was written and is that mix, false when
        `room` already is. Both buffers are stereo. `inputs` may be null or hold
        null channels; null channels are skipped when counting pairs, and a
        missing pair is silent. A mono source sums its pair rather than
        averaging it: inputs are offered in pairs, and a mic plugged into
        either half of one should arrive at full level, not at half.

        `recordingIsSeparate` says an earlier source already built `recording`
        as a mix of its own. This one is then added to it as well as to the
        room, so two sources kept out of the speakers both reach the stream.

        Audio thread: allocates nothing, locks nothing. */
    bool processBlock (const float* const* inputs, int numInputChannels,
                       juce::AudioBuffer<float>& room,
                       juce::AudioBuffer<float>& recording,
                       int numSamples,
                       bool recordingIsSeparate = false) noexcept;

private:
    const Source source;
    std::atomic<int> inputPair { 0 };
    std::atomic<bool> enabled { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<bool> talkover { false };
    std::atomic<Routing> routing { Routing::everywhere };
    std::atomic<float> peak { 0.0f };

    // Audio thread only.
    float envelope = 0.0f;        // 0 is off, 1 fully on
    float duckGain = 1.0f;        // applied to the music
    float smoothedGain = 1.0f;
    float envelopeStep = 0.0f;
    float duckDownStep = 0.0f;
    float duckUpStep = 0.0f;
    float gainStep = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicInput)
};

} // namespace opendj
