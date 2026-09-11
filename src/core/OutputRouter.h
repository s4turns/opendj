/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace opendj
{

/** How the master and cue busses reach the device's outputs. */
enum class OutputMode
{
    /** Master on the first output pair, cue on the second. What every DJ
        interface is wired for, and what a controller with its own four channel
        interface presents. Needs four outputs; with two, there is nowhere for
        the cue bus to go and it is dropped. */
    separatePairs,

    /** Master on the first output, cue on the second, both summed to mono.

        This is the trick that predates DJ interfaces: one stereo output, a
        splitter cable, and the two halves going to the speakers and the
        headphones. It costs stereo in both, which is why it is never the
        default, but it is the difference between being able to pre-listen on a
        plain sound card and not. */
    splitStereo
};

/** Writes the master and cue busses to the device outputs.

    Realtime safe: no allocation, no locks, and never a write past `numSamples`
    or past the channel the device gave us. A null channel pointer is skipped,
    since JUCE passes null for an output that is not active.
*/
void routeOutputs (const juce::AudioBuffer<float>& master,
                   const juce::AudioBuffer<float>& cue,
                   float* const* outputs,
                   int numOutputChannels,
                   int numSamples,
                   OutputMode mode) noexcept;

/** Whether the cue bus can actually be heard, given a mode and a device.

    The honest version of "has a cue output": separate pairs need four outputs,
    a split needs two. Somewhere to ask before telling the user their headphones
    are useless. */
bool cueIsAudible (OutputMode mode, int numOutputChannels) noexcept;

/** The name a settings file stores, and the reverse. Unknown text reads as
    separate pairs, which is the safe answer: it never puts a mono master into
    one speaker without being asked. */
juce::String toString (OutputMode mode);
OutputMode outputModeFromString (const juce::String& name);

} // namespace opendj
