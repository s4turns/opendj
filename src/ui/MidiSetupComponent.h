/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "control/MidiControlSurface.h"

namespace opendj
{

/** Choose a controller, choose a mapping, and watch what it sends.

    The monitor is not a debugging afterthought. It is how anyone verifies a
    mapping against real hardware, and how a user with a controller nobody has
    mapped yet finds out what their buttons send.
*/
class MidiSetupComponent final : public juce::Component,
                                 private juce::Timer,
                                 private juce::ListBoxModel
{
public:
    explicit MidiSetupComponent (MidiControlSurface& surfaceToUse);
    ~MidiSetupComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshDeviceList();
    void refreshMappingList();

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;

    MidiControlSurface& surface;

    juce::Label deviceLabel, mappingLabel, statusLabel;
    juce::ComboBox deviceBox, mappingBox;
    juce::TextButton rescanButton { "Rescan" };
    juce::TextButton clearButton { "Clear log" };
    juce::ListBox messageList { "MIDI messages", this };
    juce::TextEditor warningsBox;

    juce::Array<juce::MidiDeviceInfo> devices;
    std::vector<MidiControlSurface::LogEntry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSetupComponent)
};

} // namespace opendj
