/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/MidiMapping.h"

#include <optional>

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
        if (text == "position14")        return ValueMode::absolutePosition14;

        return fallback;
    }

    /** A raw MIDI message written as hex bytes, for example "F0 00 20 7F 00 F7"
        or "BF 64 00". Returns nothing if it is not a message that can be sent. */
    std::optional<juce::MidiMessage> parseRawMessage (const juce::String& text)
    {
        juce::StringArray tokens;
        tokens.addTokens (text, " ,\t", {});
        tokens.removeEmptyStrings();

        std::vector<juce::uint8> bytes;

        for (auto token : tokens)
        {
            token = token.startsWithIgnoreCase ("0x") ? token.substring (2) : token;

            if (! token.containsOnly ("0123456789abcdefABCDEF") || token.isEmpty())
                return {};

            const auto value = token.getHexValue32();

            if (value < 0 || value > 255)
                return {};

            bytes.push_back (static_cast<juce::uint8> (value));
        }

        if (bytes.empty())
            return {};

        if (bytes.front() == 0xF0)
        {
            // JUCE wraps the payload itself, so hand it what is between the
            // start and end bytes.
            if (bytes.size() < 3 || bytes.back() != 0xF7)
                return {};

            return juce::MidiMessage::createSysExMessage (bytes.data() + 1,
                                                          static_cast<int> (bytes.size()) - 2);
        }

        switch (bytes.size())
        {
            case 1:  return juce::MidiMessage (bytes[0]);
            case 2:  return juce::MidiMessage (bytes[0], bytes[1]);
            case 3:  return juce::MidiMessage (bytes[0], bytes[1], bytes[2]);
            default: return {};
        }
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

    const auto readMessages = [&warnings] (const juce::var& value, const juce::String& what)
    {
        std::vector<juce::MidiMessage> messages;

        const auto readOne = [&] (const juce::var& entry)
        {
            if (const auto message = parseRawMessage (entry.toString()))
                messages.push_back (*message);
            else
                warnings.add (what + ": could not read the message \"" + entry.toString() + "\".");
        };

        if (const auto* array = value.getArray())
            for (const auto& entry : *array)
                readOne (entry);
        else if (value.toString().isNotEmpty())
            readOne (value);

        return messages;
    };

    destination.initMessages = readMessages (json.getProperty ("initMessages", {}), "initMessages");

    if (const auto keepAlive = json.getProperty ("keepAlive", {}); keepAlive.isObject())
    {
        destination.keepAliveMessages = readMessages (keepAlive.getProperty ("message", {}), "keepAlive");

        if (const auto interval = parseNumber (keepAlive.getProperty ("intervalMs", {})))
            destination.keepAliveIntervalMs = juce::jlimit (10, 10000, *interval);
        else if (! destination.keepAliveMessages.empty())
            destination.keepAliveIntervalMs = 500;
    }

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

        if (const auto ticks = parseNumber (entry.getProperty ("ticksPerRevolution", {})))
            control.ticksPerRevolution = juce::jmax (1, *ticks);

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
