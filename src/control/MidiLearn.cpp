/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/MidiLearn.h"

#include <cstdlib>

namespace opendj
{

namespace
{
    /** Actions that count movement rather than go to a position. */
    bool wantsRelative (Action action) noexcept
    {
        return action == Action::jogTurn || action == Action::browseScroll || action == Action::deckWaveformZoom;
    }

    // How far from its resting value an encoder's steps go. Well past what a hand
    // does, and short of what a fader sweeping its travel covers.
    constexpr int encoderSpread = 24;
}

juce::String nameForLearnedControl (const LearnTarget& target)
{
    juce::String name;

    if (actionTakesDeck (target.action))
        name << "Deck " << juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, 25, target.deck))) << ": ";

    name << toString (target.action);

    if (actionTakesSlot (target.action))
        name << " " << (target.slot + 1);

    return name;
}

MidiControl MidiLearner::makeControl (int status, int number, ValueMode mode) const
{
    MidiControl control;
    control.name = nameForLearnedControl (target);
    control.status = status;
    control.number = number;
    control.action = target.action;
    control.deck = target.deck;
    control.slot = target.slot;
    control.mode = mode;
    return control;
}

std::optional<MidiControl> MidiLearner::feed (int status, int number, int value)
{
    const auto type = status & 0xF0;

    switch (type)
    {
        case 0x90:
        {
            // The release is not the control being touched. A note on at no
            // velocity is how a lot of hardware sends a release.
            if (value == 0)
                return {};

            // Its velocity is what padMode reads. Anything else takes a note as
            // a press, even a continuous action: 1 down, and 0 up.
            auto control = makeControl (status, number,
                                        target.action == Action::padMode ? ValueMode::absolute : ValueMode::button);

            // Most controllers light a button with the note it sends. One with no
            // such light ignores it.
            control.feedbackNumber = number;
            return control;
        }

        case 0xE0:
            // A pitch bend has no number, which a mapping writes as 0. It says
            // where something is, and only a jog wheel wants the difference.
            return makeControl (status, 0, target.action == Action::jogTurn ? ValueMode::absolutePosition14
                                         : isContinuous (target.action)     ? ValueMode::pitchBend14
                                                                            : ValueMode::button);

        case 0xB0:
            break;

        default:
            // A note off, aftertouch, a program change or anything else no
            // control on a DJ controller is learned from.
            return {};
    }

    // A control change as a button: its value says down or up, and down is enough.
    if (! isContinuous (target.action))
        return value > 0 ? std::optional<MidiControl> (makeControl (status, number, ValueMode::button)) : std::nullopt;

    const auto key = std::make_pair (status, number);

    if (seen.empty())
        first = key;

    auto& entry = seen[key];
    ++entry.count;
    entry.lowest = juce::jmin (entry.lowest, value);
    entry.highest = juce::jmax (entry.highest, value);
    entry.nearCentre = entry.nearCentre && std::abs (value - 64) <= encoderSpread;
    entry.nearEnds = entry.nearEnds && (value <= encoderSpread || value >= 128 - encoderSpread);

    if (entry.count >= messagesToDecide)
        return classify();

    return {};
}

std::optional<MidiControl> MidiLearner::finish() const
{
    return classify();
}

std::optional<MidiControl> MidiLearner::classify() const
{
    if (seen.empty())
        return {};

    const auto status = first.first;
    auto number = first.second;

    // The coarse and fine halves of one fader arrive on number n and n + 32.
    // Whichever turned up first, the pair is named by its coarse half. A relative
    // action cannot use a position in any resolution.
    if (! wantsRelative (target.action))
    {
        const auto partner = number < 32 ? number + 32 : number - 32;

        if (partner >= 0 && partner < 64 && seen.count ({ status, partner }) != 0)
        {
            const auto coarse = juce::jmin (number, partner);
            auto control = makeControl (status, coarse, ValueMode::absolute14Bit);
            control.fineNumber = coarse + 32;
            return control;
        }

        return makeControl (status, number, ValueMode::absolute);
    }

    const auto& entry = seen.at (first);

    // Steps either side of 64, or just above 0 and just below 128. A control
    // that fits neither has been told to count something it cannot, and the
    // second reading of it is the less harmful: steps stay small.
    return makeControl (status, number, entry.nearCentre && ! entry.nearEnds ? ValueMode::relativeOffset
                                      : ValueMode::relativeTwosComplement);
}

} // namespace opendj
