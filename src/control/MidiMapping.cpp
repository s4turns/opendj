/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/MidiMapping.h"

namespace opendj
{

namespace
{
    /** Accepts either a number or a "0x90" style string, because a mapping file
        is written by hand and hexadecimal is how MIDI is always documented. */
    std::optional<int> parseNumber (const juce::var& value)
    {
        if (value.isVoid() || value.isUndefined())
            return {};

        if (value.isInt() || value.isInt64() || value.isDouble())
            return static_cast<int> (value);

        if (value.isString())
        {
            const auto text = value.toString().trim();

            if (text.startsWithIgnoreCase ("0x"))
                return text.substring (2).getHexValue32();

            if (text.containsOnly ("0123456789") && text.isNotEmpty())
                return text.getIntValue();
        }

        return {};
    }

    ValueMode parseMode (const juce::String& text, ValueMode fallback)
    {
        if (text == "button")            return ValueMode::button;
        if (text == "absolute")          return ValueMode::absolute;
        if (text == "absolute14")        return ValueMode::absolute14Bit;
        if (text == "relative_offset")   return ValueMode::relativeOffset;
        if (text == "relative_twos")     return ValueMode::relativeTwosComplement;

        return fallback;
    }
}

juce::Result MidiMapping::loadFromFile (const juce::File& file,
                                        MidiMapping& destination,
                                        juce::StringArray& warnings)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("Mapping file not found: " + file.getFullPathName());

    juce::var json;
    const auto parsed = juce::JSON::parse (file.loadFileAsString(), json);

    if (parsed.failed())
        return juce::Result::fail (file.getFileName() + ": " + parsed.getErrorMessage());

    return loadFromJson (json, destination, warnings);
}

juce::Result MidiMapping::loadFromJson (const juce::var& json,
                                        MidiMapping& destination,
                                        juce::StringArray& warnings)
{
    auto* root = json.getDynamicObject();

    if (root == nullptr)
        return juce::Result::fail ("Mapping is not a JSON object.");

    destination = {};
    destination.name = json.getProperty ("name", "Unnamed mapping").toString();
    destination.author = json.getProperty ("author", {}).toString();
    destination.description = json.getProperty ("description", {}).toString();

    if (const auto ticks = parseNumber (json.getProperty ("jogTicksPerRevolution", {})))
        destination.jogTicksPerRevolution = juce::jmax (1, *ticks);

    if (const auto* hints = json.getProperty ("deviceNameHints", {}).getArray())
        for (const auto& hint : *hints)
            destination.deviceNameHints.add (hint.toString());

    const auto* controls = json.getProperty ("controls", {}).getArray();

    if (controls == nullptr)
        return juce::Result::fail ("Mapping has no \"controls\" array.");

    for (int i = 0; i < controls->size(); ++i)
    {
        const auto& entry = controls->getReference (i);
        const auto label = entry.getProperty ("name", "control " + juce::String (i)).toString();

        const auto status = parseNumber (entry.getProperty ("status", {}));
        const auto number = parseNumber (entry.getProperty ("number", {}));

        if (! status.has_value() || ! number.has_value())
        {
            warnings.add (label + ": missing or unreadable \"status\" or \"number\".");
            continue;
        }

        const auto actionName = entry.getProperty ("action", {}).toString();
        const auto action = actionFromString (actionName);

        if (action == Action::none)
        {
            warnings.add (label + ": unknown action \"" + actionName + "\".");
            continue;
        }

        MidiControl control;
        control.name = label;
        control.status = *status;
        control.number = *number;
        control.action = action;

        if (const auto fine = parseNumber (entry.getProperty ("fineNumber", {})))
            control.fineNumber = *fine;

        if (const auto deck = parseNumber (entry.getProperty ("deck", {})))
            control.deck = *deck;

        if (const auto slot = parseNumber (entry.getProperty ("slot", {})))
            control.slot = *slot;

        // A continuous control defaults to absolute and a button to button, so
        // the common cases need no "mode" line at all.
        control.mode = parseMode (entry.getProperty ("mode", {}).toString(),
                                  isContinuous (action) ? ValueMode::absolute : ValueMode::button);

        if (control.fineNumber >= 0 && control.mode == ValueMode::absolute)
            control.mode = ValueMode::absolute14Bit;

        control.inverted = static_cast<bool> (entry.getProperty ("inverted", false));
        control.requiresShift = static_cast<bool> (entry.getProperty ("shift", false));

        if (const auto feedback = parseNumber (entry.getProperty ("feedbackNumber", {})))
            control.feedbackNumber = *feedback;

        if (const auto velocity = parseNumber (entry.getProperty ("feedbackOnVelocity", {})))
            control.feedbackOnVelocity = *velocity;

        if (const auto velocity = parseNumber (entry.getProperty ("feedbackOffVelocity", {})))
            control.feedbackOffVelocity = *velocity;

        destination.controls.push_back (std::move (control));
    }

    if (destination.controls.empty())
        return juce::Result::fail ("Mapping defined no usable controls.");

    return juce::Result::ok();
}

const MidiControl* MidiMapping::findControl (int status, int number, bool shiftHeld) const
{
    const MidiControl* unshifted = nullptr;

    for (const auto& control : controls)
    {
        if (control.status != status)
            continue;

        // A 14-bit control answers to both halves of its pair.
        if (control.number != number && control.fineNumber != number)
            continue;

        if (control.requiresShift)
        {
            if (shiftHeld)
                return &control;   // an exact match on the modifier wins outright
        }
        else if (unshifted == nullptr)
        {
            unshifted = &control;
        }
    }

    return unshifted;
}

bool MidiMapping::matchesDeviceName (const juce::String& deviceName) const
{
    for (const auto& hint : deviceNameHints)
        if (hint.isNotEmpty() && deviceName.containsIgnoreCase (hint))
            return true;

    return false;
}

} // namespace opendj
