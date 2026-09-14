/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "control/MidiControlSurface.h"

#include <algorithm>
#include <iostream>

namespace opendj
{

namespace
{
    constexpr int feedbackIntervalMs = 50;

    /** Set OPENDJ_MIDI_TRACE=1 to have every incoming message printed, with the
        control it matched. A controller that does nothing is otherwise very
        hard to argue with: this says whether the messages are arriving at all,
        and if they are, what the mapping made of them. */
    bool midiTracingEnabled()
    {
        static const auto enabled =
            juce::SystemStats::getEnvironmentVariable ("OPENDJ_MIDI_TRACE", {}).getIntValue() != 0;

        return enabled;
    }

    /** Actions that quietly do nothing when the deck is empty. */
    bool needsLoadedTrack (Action action)
    {
        switch (action)
        {
            case Action::deckPlayToggle:
            case Action::deckPlay:
            case Action::deckPause:
            case Action::deckCue:
            case Action::deckSync:
            case Action::deckSeek:
            case Action::jogTouch:
            case Action::jogTurn:
            case Action::hotCue:
            case Action::hotCueClear:
                return true;

            default:
                return false;
        }
    }

    void trace (const juce::String& line)
    {
        if (midiTracingEnabled())
        {
            std::cerr << "[opendj midi] " << line << std::endl;
            std::cerr.flush();
        }
    }

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
    trace ("opened input \"" + openDeviceName + "\"");

    // Pair it with an output of the same name so the lights work. A controller
    // with no output, or one already claimed elsewhere, still plays fine; it
    // just has dark buttons.
    for (const auto& output : availableOutputs())
    {
        if (output.name == openDeviceName)
        {
            midiOutput = juce::MidiOutput::openDevice (output.identifier);
            trace (midiOutput != nullptr ? "opened output \"" + output.name + "\""
                                         : "FAILED to open output \"" + output.name + "\"");
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

    trace (selectedMapping >= 0
            ? "selected mapping \"" + mappings[(size_t) selectedMapping].name + "\""
            : juce::String ("NO MAPPING matched this device"));

    if (midiOutput == nullptr)
        trace ("no MIDI output: a controller needing a handshake will stay silent");

    lastFeedbackState.clear();
    lastPlatterPosition.fill (-1);
    sendInitMessages();
    refreshFeedback();
    return juce::Result::ok();
}

void MidiControlSurface::sendInitMessages()
{
    if (midiOutput == nullptr
        || ! juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
        return;

    const auto& mapping = mappings[(size_t) selectedMapping];

    trace ("sending " + juce::String ((int) mapping.initMessages.size())
            + " init message(s) for \"" + mapping.name + "\"");

    for (const auto& message : mapping.initMessages)
        midiOutput->sendMessageNow (message);

    // Start the clock now, so the first reminder is due an interval from the
    // request rather than immediately after it.
    lastKeepAliveMs = juce::Time::getMillisecondCounter();
}

void MidiControlSurface::sendKeepAlive()
{
    if (midiOutput == nullptr
        || ! juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
        return;

    const auto& mapping = mappings[(size_t) selectedMapping];

    if (mapping.keepAliveIntervalMs <= 0 || mapping.keepAliveMessages.empty())
        return;

    const auto now = juce::Time::getMillisecondCounter();

    if (now - lastKeepAliveMs < static_cast<juce::uint32> (mapping.keepAliveIntervalMs))
        return;

    lastKeepAliveMs = now;

    for (const auto& message : mapping.keepAliveMessages)
        midiOutput->sendMessageNow (message);
}

bool MidiControlSurface::needsUnavailableOutput() const
{
    if (midiOutput != nullptr
        || ! juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
        return false;

    const auto& mapping = mappings[(size_t) selectedMapping];
    return ! mapping.initMessages.empty() || ! mapping.keepAliveMessages.empty();
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

        // Folders are searched in order of precedence, so a mapping the user
        // wrote themselves takes the place of the one that shipped with the
        // same name rather than appearing beside it.
        if (std::any_of (mappings.begin(), mappings.end(),
                         [&mapping] (const MidiMapping& existing) { return existing.name == mapping.name; }))
            continue;

        for (const auto& warning : fileWarnings)
            warnings.add (mapping.name + ": " + warning);

        mappings.push_back (std::move (mapping));
        ++loaded;
    }

    if (selectedMapping < 0 && ! mappings.empty())
        selectedMapping = 0;

    return loaded;
}

juce::StringArray MidiControlSurface::getDeviceNameHints() const
{
    juce::StringArray hints;

    // The open device first, so a controller that is actually plugged in wins
    // over one that merely has a mapping on disk.
    if (openDeviceName.isNotEmpty())
        hints.add (openDeviceName);

    for (const auto& mapping : mappings)
        for (const auto& hint : mapping.deviceNameHints)
            hints.addIfNotAlreadyThere (hint);

    return hints;
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
            sendInitMessages();
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

    // Pitch bend has no controller number: its second byte is the low half of
    // the value and changes constantly, so a mapping addresses it by status
    // alone and every such entry is written with number 0.
    const auto isPitchBend = (status & 0xF0) == 0xE0;
    const auto lookupNumber = isPitchBend ? 0 : number;
    const auto isNoteOff = (status & 0xF0) == 0x80;

    if (juce::isPositiveAndBelow (selectedMapping, static_cast<int> (mappings.size())))
    {
        const auto& mapping = mappings[(size_t) selectedMapping];
        control = mapping.findControl (status, lookupNumber, dispatcher.isShiftHeld());

        // A button that was pressed with a note on is released with a note off,
        // and a mapping names it once, at the note on. Without this the release
        // matches nothing and is dropped: the DJ-202's platter then never stops
        // being touched, so the deck stays in a scratch that never ends, and a
        // cue button never comes back up.
        if (control == nullptr && isNoteOff)
            control = mapping.findControl (status + 0x10, lookupNumber, dispatcher.isShiftHeld());
    }

    {
        std::lock_guard<std::mutex> lock (logMutex);
        log.push_front ({ describe (message), control != nullptr ? control->name : "unmapped" });

        while (log.size() > maxLogEntries)
            log.pop_back();
    }

    if (midiTracingEnabled())
    {
        auto line = describe (message) + "  ->  "
                  + (control != nullptr ? control->name + " (" + toString (control->action) + ")"
                                        : juce::String ("unmapped"));

        // The commonest reason a controller looks dead: the transport and the
        // platters do nothing at all on a deck with no track on it, while the
        // EQ and the faders carry on working. Saying so here saves the guess.
        if (isNoteOff)
            line << "   [release]";

        if (control != nullptr && needsLoadedTrack (control->action)
            && ! engine.getDeck (juce::jlimit (0, AudioEngine::numDecks - 1, control->deck)).isLoaded())
            line << "   [ignored: deck " << control->deck << " has no track loaded]";

        trace (line);
    }

    if (control != nullptr)
        handleMappedMessage (*control, lookupNumber, rawValue, number, isNoteOff);
}

float MidiControlSurface::valueFor (const MidiControl& control, int number, int rawValue, int lowByte)
{
    switch (control.mode)
    {
        case ValueMode::absolutePosition14:
        {
            // Pitch bend is sent low byte first, so the position is the two
            // seven-bit halves put back together.
            const auto position = ((rawValue & 0x7F) << 7) | (lowByte & 0x7F);
            const auto channel = control.status & 0x0F;
            auto& last = lastPlatterPosition[(size_t) channel];

            if (last < 0)
            {
                // The first reading only says where the platter is, not that it
                // moved. Reporting the difference from nothing would fling the
                // track across the room.
                last = position;
                return 0.0f;
            }

            auto delta = position - last;
            last = position;

            // A platter goes round: crossing zero is a small movement, not a
            // leap the length of the whole scale.
            constexpr int range = 1 << 14;

            if (delta > range / 2)
                delta -= range;
            else if (delta < -range / 2)
                delta += range;

            return static_cast<float> (delta) * (control.inverted ? -1.0f : 1.0f);
        }

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

void MidiControlSurface::handleMappedMessage (const MidiControl& control, int number, int rawValue,
                                              int lowByte, bool isRelease)
{
    // A note off is a release whatever velocity it carries. The message says so,
    // not the mapping: the same control is named once and answers to both.
    const auto effectiveValue = isRelease || (control.status & 0xF0) == 0x80 ? 0 : rawValue;

    const auto value = valueFor (control, number, effectiveValue, lowByte);

    if (value < 0.0f && control.mode == ValueMode::absolute14Bit)
        return;   // half of a pair, nothing to do yet

    if (control.action == Action::jogTurn)
    {
        // A control may count in its own units; otherwise the mapping's figure
        // stands. Whichever platter source the hardware actually uses is the
        // one that sets this, so a mapping can carry both.
        const auto& mapping = mappings[(size_t) selectedMapping];
        const auto ticks = control.ticksPerRevolution > 0 ? control.ticksPerRevolution
                                                          : mapping.jogTicksPerRevolution;
        engine.getDeck (control.deck).setJogTicksPerRevolution (ticks);
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

        case Action::deckKeyLockToggle:
            return deck.isKeyLockEnabled();

        case Action::loopIn:
        case Action::loopOut:
        case Action::loopToggle:
            // Both ends of the loop section light while a loop is running, so
            // the state is visible without looking at the screen.
            return deck.isLoopEnabled();

        case Action::hotCue:
            return deck.hasHotCue (control.slot);

        case Action::channelCue:
        case Action::channelCueToggle:
            return const_cast<AudioEngine&> (engine).getMixer().isChannelCued (deckIndex);

        case Action::channelFxSelect:
            // Lights the button for whichever effect the shared depth knob is
            // currently reaching, so a glance at the FX section says which one
            // a turn of the knob will move.
            return dispatcher.getFxDepthTarget (deckIndex) == control.slot;

        case Action::micToggle:
            // Lit while the mic is live, which is the one light that matters
            // most not to miss.
            return const_cast<AudioEngine&> (engine).getMic().isEnabled();

        case Action::micTalkover:
            return const_cast<AudioEngine&> (engine).getMic().isTalkoverEnabled();

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
    // Before the lights: a controller that has dropped back into standalone
    // mode is not listening to them anyway.
    sendKeepAlive();
    refreshFeedback();
}

} // namespace opendj
