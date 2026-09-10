/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "control/ActionDispatcher.h"
#include "control/MidiMapping.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace opendj
{

/** Connects a MIDI controller to the action dispatcher.

    Input arrives on JUCE's MIDI thread and is turned into actions immediately,
    because a jog wheel that waits for the message thread feels like a jog wheel
    that is broken. LED feedback goes the other way on a timer, in batches: the
    DJ-202 has twenty-odd lights, and sending them one at a time as state changes
    would flood the port.
*/
class MidiControlSurface final : private juce::MidiInputCallback,
                                 private juce::Timer
{
public:
    MidiControlSurface (AudioEngine& engineToUse, ActionDispatcher& dispatcherToUse);
    ~MidiControlSurface() override;

    //==========================================================================
    // Devices
    //==========================================================================

    static juce::Array<juce::MidiDeviceInfo> availableInputs();
    static juce::Array<juce::MidiDeviceInfo> availableOutputs();

    /** Opens an input, and an output with a matching name if there is one.
        Passing an empty identifier closes whatever is open. */
    juce::Result openDevice (const juce::String& inputIdentifier);
    void closeDevice();

    juce::String getOpenDeviceName() const;
    bool isOpen() const;
    bool hasFeedbackOutput() const { return midiOutput != nullptr; }

    /** Opens the first connected device that any loaded mapping recognises.
        Returns the device name, or an empty string if nothing matched. */
    juce::String openFirstRecognisedDevice();

    //==========================================================================
    // Mappings
    //==========================================================================

    /** Loads every .json mapping in a folder. Returns how many loaded. */
    int loadMappingsFromFolder (const juce::File& folder);

    juce::StringArray getMappingNames() const;

    /** Every device name the loaded mappings recognise. A controller's audio
        interface is named after the same hardware as its MIDI port, so this is
        also the list of interfaces worth preferring. */
    juce::StringArray getDeviceNameHints() const;
    juce::StringArray getWarnings() const { return warnings; }

    /** Selects a mapping by name. An empty name means no mapping, in which case
        messages still reach the monitor but do nothing. */
    bool selectMapping (const juce::String& mappingName);
    juce::String getSelectedMappingName() const;

    //==========================================================================
    // Monitoring and learn
    //==========================================================================

    struct LogEntry
    {
        juce::String description;   ///< the raw message, formatted for reading
        juce::String mappedTo;      ///< the control it hit, or "unmapped"
    };

    /** The most recent messages, newest first. */
    std::vector<LogEntry> getRecentMessages() const;
    void clearLog();

    /** While a learn callback is set, incoming messages are reported to it
        instead of being dispatched. */
    void startLearning (std::function<void (int status, int number)> callback);
    void stopLearning();
    bool isLearning() const;

    /** Refreshes every mapped LED from the current engine state. */
    void refreshFeedback();

    /** True when the mapping asks for messages the device never got, because
        there is no output to send them on. Such a controller stays silent, and
        the setup panel says so rather than leaving it a mystery. */
    bool needsUnavailableOutput() const;

private:
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;
    void timerCallback() override;

    void handleMappedMessage (const MidiControl& control, int number, int rawValue,
                              int lowByte, bool isRelease);
    void sendInitMessages();
    void sendKeepAlive();
    float valueFor (const MidiControl& control, int number, int rawValue, int lowByte);
    bool feedbackStateFor (const MidiControl& control) const;

    static juce::String describe (const juce::MidiMessage& message);

    AudioEngine& engine;
    ActionDispatcher& dispatcher;

    std::unique_ptr<juce::MidiInput> midiInput;
    std::unique_ptr<juce::MidiOutput> midiOutput;
    juce::String openDeviceName;

    std::vector<MidiMapping> mappings;
    int selectedMapping = -1;
    juce::StringArray warnings;

    // Coarse halves of 14-bit pairs, held until the fine half arrives.
    std::array<int, 128 * 16> coarseValues {};

    // The last absolute platter reading per channel, so movement can be worked
    // out from the difference. Negative means nothing has been read yet.
    std::array<int, 16> lastPlatterPosition { -1, -1, -1, -1, -1, -1, -1, -1,
                                              -1, -1, -1, -1, -1, -1, -1, -1 };

    mutable std::mutex logMutex;
    std::deque<LogEntry> log;
    static constexpr size_t maxLogEntries = 200;

    mutable std::mutex learnMutex;
    std::function<void (int, int)> learnCallback;

    // Last state sent for each feedback light, so only changes go out.
    std::map<int, bool> lastFeedbackState;

    juce::uint32 lastKeepAliveMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiControlSurface)
};

} // namespace opendj
