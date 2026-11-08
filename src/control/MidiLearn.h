/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "control/MidiMapping.h"

#include <map>
#include <optional>

namespace opendj
{

/** What a control is being learned for. */
struct LearnTarget
{
    Action action = Action::none;
    int deck = 0;
    int slot = 0;
};

/** The name a learned control is given, such as "Deck B: deck.play_toggle". */
juce::String nameForLearnedControl (const LearnTarget& target);

/** Works out what kind of control has been touched, from the messages it sends.

    A press answers at once. A fader or a knob needs a few messages before it can
    be told from anything else: a relative encoder only looks like one once it has
    been turned, and one half of a 14-bit pair looks just like a plain control
    change until the other half turns up.

    Plain logic with no device and no threads, so that it can be tested message by
    message. MidiControlSurface feeds it and keeps it locked.
*/
class MidiLearner
{
public:
    explicit MidiLearner (LearnTarget targetToLearn) : target (targetToLearn) {}

    /** One incoming message. Returns the control once it is sure of it. */
    std::optional<MidiControl> feed (int status, int number, int value);

    /** The control has stopped moving: gives the best answer from what was seen,
        or nothing if it saw nothing it could use. */
    std::optional<MidiControl> finish() const;

    /** True once there is something that finish() could answer from. */
    bool hasSeenAnything() const noexcept { return ! seen.empty(); }

    const LearnTarget& getTarget() const noexcept { return target; }

    /** How many messages of one control change it wants before it decides. */
    static constexpr int messagesToDecide = 6;

private:
    struct Seen
    {
        int count = 0;
        int lowest = 127;
        int highest = 0;
        bool nearCentre = true;     // every value close to 64
        bool nearEnds = true;       // every value close to 0 or to 127
    };

    MidiControl makeControl (int status, int number, ValueMode mode) const;
    std::optional<MidiControl> classify() const;

    LearnTarget target;
    std::map<std::pair<int, int>, Seen> seen;   // by status and number
    std::pair<int, int> first { -1, -1 };
};

} // namespace opendj
