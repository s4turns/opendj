/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "control/Action.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>
#include <vector>

namespace opendj
{

/** How a control's raw MIDI value becomes an action value. */
enum class ValueMode
{
    button,             ///< note on or off, or a control change used as a switch
    absolute,           ///< 0 to 127 mapped onto 0 to 1
    absolute14Bit,      ///< a coarse and fine control change pair, 0 to 16383
    relativeOffset,     ///< signed, centred on 64: the DJ-202 jog wheels
    relativeTwosComplement,

    /** A 14-bit absolute position, sent as pitch bend, that wraps round rather
        than stopping at either end: a platter, in other words. Movement is the
        difference between one reading and the last, so the first reading after
        a hand lands only establishes where the platter was. */
    absolutePosition14
};

/** One physical control and what it does. */
struct MidiControl
{
    juce::String name;          ///< for the monitor and for error messages
    int status = 0x90;          ///< message type and channel, e.g. 0x90 or 0xB0
    int number = 0;             ///< note number or control change number
    int fineNumber = -1;        ///< the fine half of a 14-bit pair, or -1

    Action action = Action::none;
    int deck = 0;
    int slot = 0;

    ValueMode mode = ValueMode::button;
    bool inverted = false;
    bool requiresShift = false;

    /** For a platter, how many of its own ticks make one revolution. Zero means
        the mapping's own figure. A wheel reporting absolute 14-bit position
        counts in far finer steps than one sending relative ticks, so the two
        cannot share a number. */
    int ticksPerRevolution = 0;

    /** LED feedback: the note to send back, or -1 for a control with no light. */
    int feedbackNumber = -1;
    int feedbackOnVelocity = 0x7F;
    int feedbackOffVelocity = 0x00;
};

/** A whole controller mapping, loaded from a JSON file.

    Keeping this as data rather than code is the point. A new controller needs a
    new file, not a new build, and the file is something a user can read, diff
    and send to someone else.
*/
class MidiMapping
{
public:
    juce::String name;
    juce::String author;
    juce::String description;

    /** Substring matched against MIDI device names to auto-select this mapping. */
    juce::StringArray deviceNameHints;

    /** Ticks the platter reports for one full revolution. */
    int jogTicksPerRevolution = 512;

    /** Sent once when the device is opened.

        Several controllers power up in a standalone mode in which they play
        their own sounds and tell the computer nothing at all. The Roland DJ-202
        is one: until it is asked, in so many words, to talk to a computer, not
        one button on it produces a MIDI message. Keeping the request here
        rather than in the code means the next such controller needs a file and
        not a release. */
    std::vector<juce::MidiMessage> initMessages;

    /** Repeated to hold the device in whatever mode initMessages put it in, and
        how often. The DJ-202 falls back to standalone about a second and a half
        after the last one. Zero means the device needs no reminding. */
    std::vector<juce::MidiMessage> keepAliveMessages;
    int keepAliveIntervalMs = 0;

    std::vector<MidiControl> controls;

    /** Parses a mapping. Returns the mapping, or an error string if the file
        could not be read or was not a mapping at all. Individual controls that
        fail to parse are skipped and reported in `warnings`, so one typo does
        not take the whole controller offline. */
    static juce::Result loadFromFile (const juce::File& file,
                                      MidiMapping& destination,
                                      juce::StringArray& warnings);

    static juce::Result loadFromJson (const juce::var& json,
                                      MidiMapping& destination,
                                      juce::StringArray& warnings);

    /** Finds the control a message belongs to, honouring the shift modifier.
        A shifted control wins over an unshifted one on the same message. */
    const MidiControl* findControl (int status, int number, bool shiftHeld) const;

    /** True when a device name looks like this mapping's controller. */
    bool matchesDeviceName (const juce::String& deviceName) const;
};

} // namespace opendj
