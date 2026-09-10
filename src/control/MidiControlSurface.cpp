/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/MidiControlSurface.h"

namespace opendj
{

namespace
{
    constexpr int feedbackIntervalMs = 50;

    int statusOf (const juce::MidiMessage& message)
    {
        const auto* data = message.getRawData();
        return message.getRawDataSize() > 0 ? data[0] : 0;
    }

    int numberOf (const juce::MidiMessage& message)
    {
        const auto* data = message.getRawData();
        return message.getRawDataSize() > 1 ? data[1] : 0;
    }

    int valueOf (const juce::MidiMessage& message)
    {
        const auto* data = message.getRawData();
        return message.getRawDataSize() > 2 ? data[2] : 0;
    }
}

MidiControlSurface::MidiControlSurface (AudioEngine& engineToUse, ActionDispatcher& dispatcherToUse)
    : engine (engineToUse), dispatcher (dispatcherToUse)
{
    coarseValues.fill (-1);
    startTimer (feedbackIntervalMs);
}

MidiControlSurface::~MidiControlSurface()
{
    stopTimer();
    closeDevice();
}

//==============================================================================
// Devices
//==============================================================================

juce::Array<juce::MidiDeviceInfo> MidiControlSurface::availableInputs()
{
    return juce::MidiInput::getAvailableDevices();
}

juce::Array<juce::MidiDeviceInfo> MidiControlSurface::availableOutputs()
{
    return juce::MidiOutput::getAvailableDevices();
}

juce::Result MidiControlSurface::openDevice (const juce::String& inputIdentifier)
{
    closeDevice();

    if (inputIdentifier.isEmpty())
        return juce::Result::ok();

    midiInput = juce::MidiInput::openDevice (inputIdentifier, this);

    if (midiInput == nullptr)
        return juce::Result::fail ("Could not open the MIDI input. Another application may have it.");

    openDeviceName = midiInput->getName();
    midiInput->start();

    // Pair it with an output of the same name so the lights work. A controller
    // with no output, or one already claimed elsewhere, still plays fine; it
    // just has dark buttons.
    for (const auto& output : availableOutputs())
    {
        if (output.name == openDeviceName)
        {
            midiOutput = juce::MidiOutput::openDevice (output.identifier);
            break;
        }
    }

    // Pick the mapping that recognises this device, if one does.
    for (size_t i = 0; i < mappings.size(); ++i)
    {
        if (mappings[i].matchesDeviceName (openDeviceName))
        {
            selectedMapping = static_cast<int> (i);
            break;
        }
    }

    lastFeedbackState.clear();
    refreshFeedback();
    return juce::Result::ok();
}

void MidiControlSurface::closeDevice()
{
    if (midiInput != nullptr)
    {
        midiInput->stop();
        midiInput.reset();
    }

    midiOutput.reset();
    openDeviceName.clear();
    lastFeedbackState.clear();
    coarseValues.fill (-1);
}

juce::String MidiControlSurface::getOpenDeviceName() const
{
    return openDeviceName;
}

bool MidiControlSurface::isOpen() const
{
    return midiInput != nullptr;
}

juce::String MidiControlSurface::openFirstRecognisedDevice()
{
    for (const auto& device : availableInputs())
    {
        for (const auto& mapping : mappings)
        {
            if (! mapping.matchesDeviceName (device.name))
                continue;

            if (openDevice (device.identifier).wasOk())
                return getOpenDeviceName();
        }
    }

    return {};
}

//==============================================================================
// Mappings
//==============================================================================

int MidiControlSurface::loadMappingsFromFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return 0;

    auto loaded = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*.json"))
    {
        MidiMapping mapping;
        juce::StringArray fileWarnings;

        if (const auto result = MidiMapping::loadFromFile (entry.getFile(), mapping, fileWarnings);
            result.failed())
        {
            warnings.add (result.getErrorMessage());
            continue;
        }

        for (const auto& warning : fileWarnings)
            warnings.add (mapping.name + ": " + warning);

        mappings.push_back (std::move (mapping));
        ++loaded;
    }

    if (selectedMapping < 0 && ! mappings.empty())
        selectedMapping = 0;

    return loaded;
}

juce::StringArray MidiControlSurface::getMappingNames() const
{
    juce::StringArray names;

    for (const auto& mapping : mappings)
        names.add (mapping.name);

    return names;
}

bool MidiControlSurface::selectMapping (const juce::String& mappingName)
{
    if (mappingName.isEmpty())
    {
        selectedMapping = -1;
        return true;
    }

    for (size_t i = 0; i < mappings.size(); ++i)
    {
        if (mappings[i].name == mappingName)
        {
            selectedMapping = static_cast<int> (i);
            lastFeedbackState.clear();
            refreshFeedback();
            return true;
        }
    }

    return false;
}

juce::String MidiControlSurface::getSelectedMappingName() const
{
    return juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size()))
        ? mappings[(size_t) selectedMapping].name
        : juce::String();
}

//==============================================================================
// Input
//==============================================================================

juce::String MidiControlSurface::describe (const juce::MidiMessage& message)
{
    const auto status = statusOf (message);
    const auto type = status & 0xF0;
    const auto channel = (status & 0x0F) + 1;

    const auto kind = [type]
    {
        switch (type)
        {
            case 0x80: return "note off";
            case 0x90: return "note on ";
            case 0xB0: return "control ";
            case 0xE0: return "pitch   ";
            default:   return "other   ";
        }
    }();

    return juce::String (kind)
         + " ch " + juce::String (channel).paddedLeft (' ', 2)
         + "  no 0x" + juce::String::toHexString (numberOf (message)).paddedLeft ('0', 2).toUpperCase()
         + "  val " + juce::String (valueOf (message)).paddedLeft (' ', 3);
}

void MidiControlSurface::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    if (message.getRawDataSize() < 2)
        return;

    const auto status = statusOf (message);
    const auto number = numberOf (message);
    const auto rawValue = valueOf (message);

    {
        std::lock_guard<std::mutex> lock (learnMutex);

        if (learnCallback != nullptr)
        {
            auto callback = learnCallback;
            juce::MessageManager::callAsync ([callback, status, number] { callback (status, number); });

            std::lock_guard<std::mutex> logLock (logMutex);
            log.push_front ({ describe (message), "learning" });

            while (log.size() > maxLogEntries)
                log.pop_back();

            return;
        }
    }

    const MidiControl* control = nullptr;

    if (juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
        control = mappings[(size_t) selectedMapping].findControl (status, number,
                                                                  dispatcher.isShiftHeld());

    {
        std::lock_guard<std::mutex> lock (logMutex);
        log.push_front ({ describe (message), control != nullptr ? control->name : "unmapped" });

        while (log.size() > maxLogEntries)
            log.pop_back();
    }

    if (control != nullptr)
        handleMappedMessage (*control, number, rawValue);
}

float MidiControlSurface::valueFor (const MidiControl& control, int number, int rawValue)
{
    switch (control.mode)
    {
        case ValueMode::button:
            return rawValue > 0 ? 1.0f : 0.0f;

        case ValueMode::absolute:
            return control.inverted ? 1.0f - rawValue / 127.0f : rawValue / 127.0f;

        case ValueMode::absolute14Bit:
        {
            // Coarse arrives first and is held; the fine half completes the pair.
            // Acting on the coarse half alone would make a fader jump in steps.
            const auto slot = (control.status & 0x0F) * 128 + control.number;

            if (number == control.number)
            {
                coarseValues[(size_t) slot] = rawValue;
                return -1.0f;   // wait for the fine half
            }

            const auto coarse = coarseValues[(size_t) slot];

            if (coarse < 0)
                return -1.0f;

            const auto combined = (coarse << 7 | rawValue) / 16383.0f;
            return control.inverted ? 1.0f - combined : combined;
        }

        case ValueMode::relativeOffset:
            // Centred on 64: above is forwards, below is backwards.
            return static_cast<float> (rawValue - 64) * (control.inverted ? -1.0f : 1.0f);

        case ValueMode::relativeTwosComplement:
            return static_cast<float> (rawValue < 64 ? rawValue : rawValue - 128)
                 * (control.inverted ? -1.0f : 1.0f);
    }

    return 0.0f;
}

void MidiControlSurface::handleMappedMessage (const MidiControl& control, int number, int rawValue)
{
    // A note off is a release whatever velocity it carries.
    const auto isNoteOff = (control.status & 0xF0) == 0x80;
    const auto effectiveValue = isNoteOff ? 0 : rawValue;

    const auto value = valueFor (control, number, effectiveValue);

    if (value < 0.0f && control.mode == ValueMode::absolute14Bit)
        return;   // half of a pair, nothing to do yet

    if (control.action == Action::jogTurn)
    {
        const auto& mapping = mappings[(size_t) selectedMapping];
        engine.getDeck (control.deck).setJogTicksPerRevolution (mapping.jogTicksPerRevolution);
    }

    dispatcher.dispatch ({ control.action, control.deck, control.slot, value });
}

//==============================================================================
// Monitoring and learn
//==============================================================================

std::vector<MidiControlSurface::LogEntry> MidiControlSurface::getRecentMessages() const
{
    std::lock_guard<std::mutex> lock (logMutex);
    return { log.begin(), log.end() };
}

void MidiControlSurface::clearLog()
{
    std::lock_guard<std::mutex> lock (logMutex);
    log.clear();
}

void MidiControlSurface::startLearning (std::function<void (int, int)> callback)
{
    std::lock_guard<std::mutex> lock (learnMutex);
    learnCallback = std::move (callback);
}

void MidiControlSurface::stopLearning()
{
    std::lock_guard<std::mutex> lock (learnMutex);
    learnCallback = nullptr;
}

bool MidiControlSurface::isLearning() const
{
    std::lock_guard<std::mutex> lock (learnMutex);
    return learnCallback != nullptr;
}

//==============================================================================
// Feedback
//==============================================================================

bool MidiControlSurface::feedbackStateFor (const MidiControl& control) const
{
    const auto deckIndex = juce::jlimit (0, AudioEngine::numDecks - 1, control.deck);
    auto& deck = const_cast<AudioEngine&> (engine).getDeck (deckIndex);

    switch (control.action)
    {
        case Action::deckPlayToggle:
        case Action::deckPlay:
            return deck.isPlaying();

        case Action::deckCue:
            return deck.isLoaded();

        case Action::deckSync:
            return const_cast<AudioEngine&> (engine).getEffectiveBpm (deckIndex) > 0.0;

        case Action::hotCue:
            return deck.hasHotCue (control.slot);

        case Action::channelCue:
        case Action::channelCueToggle:
            return const_cast<AudioEngine&> (engine).getMixer().isChannelCued (deckIndex);

        default:
            return false;
    }
}

void MidiControlSurface::refreshFeedback()
{
    if (midiOutput == nullptr
        || ! juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
        return;

    const auto& mapping = mappings[(size_t) selectedMapping];
    juce::MidiBuffer batch;
    auto sample = 0;

    for (const auto& control : mapping.controls)
    {
        if (control.feedbackNumber < 0)
            continue;

        const auto state = feedbackStateFor (control);
        const auto key = (control.status & 0x0F) * 128 + control.feedbackNumber;

        if (const auto existing = lastFeedbackState.find (key);
            existing != lastFeedbackState.end() && existing->second == state)
            continue;   // nothing changed, so say nothing

        lastFeedbackState[key] = state;

        const auto channel = (control.status & 0x0F) + 1;
        const auto velocity = state ? control.feedbackOnVelocity : control.feedbackOffVelocity;

        batch.addEvent (juce::MidiMessage::noteOn (channel, control.feedbackNumber,
                                                   static_cast<juce::uint8> (velocity)),
                        sample++);
    }

    if (! batch.isEmpty())
        midiOutput->sendBlockOfMessagesNow (batch);
}

void MidiControlSurface::timerCallback()
{
    refreshFeedback();
}

} // namespace opendj
