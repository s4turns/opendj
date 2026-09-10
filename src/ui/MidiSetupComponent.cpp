/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MidiSetupComponent.h"

namespace opendj
{

namespace
{
    const juce::Colour panelColour { 0xff1c1c22 };
    const juce::Colour mappedColour { 0xff35c2f0 };
}

MidiSetupComponent::MidiSetupComponent (MidiControlSurface& surfaceToUse)
    : surface (surfaceToUse)
{
    const auto setUpLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colours::grey);
        label.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (label);
    };

    setUpLabel (deviceLabel, "Controller");
    setUpLabel (mappingLabel, "Mapping");

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    deviceBox.onChange = [this]
    {
        const auto selected = deviceBox.getSelectedId();

        if (selected <= 1)
        {
            surface.closeDevice();
        }
        else if (const auto result = surface.openDevice (devices[selected - 2].identifier);
                 result.failed())
        {
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "Could not open controller",
                                                         result.getErrorMessage());
        }

        refreshMappingList();
    };
    addAndMakeVisible (deviceBox);

    mappingBox.onChange = [this]
    {
        surface.selectMapping (mappingBox.getSelectedId() <= 1 ? juce::String()
                                                               : mappingBox.getText());
    };
    addAndMakeVisible (mappingBox);

    rescanButton.onClick = [this] { refreshDeviceList(); };
    addAndMakeVisible (rescanButton);

    clearButton.onClick = [this] { surface.clearLog(); };
    addAndMakeVisible (clearButton);

    messageList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff101014));
    messageList.setRowHeight (18);
    addAndMakeVisible (messageList);

    warningsBox.setMultiLine (true);
    warningsBox.setReadOnly (true);
    warningsBox.setScrollbarsShown (true);
    warningsBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101014));
    warningsBox.setColour (juce::TextEditor::textColourId, juce::Colours::orange);
    warningsBox.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    addAndMakeVisible (warningsBox);

    refreshDeviceList();
    refreshMappingList();

    const auto warnings = surface.getWarnings();
    warningsBox.setText (warnings.isEmpty() ? "All mappings loaded cleanly."
                                            : warnings.joinIntoString ("\n"),
                         false);

    startTimerHz (10);
    setSize (620, 520);
}

MidiSetupComponent::~MidiSetupComponent()
{
    stopTimer();
}

void MidiSetupComponent::refreshDeviceList()
{
    devices = MidiControlSurface::availableInputs();

    deviceBox.clear (juce::dontSendNotification);
    deviceBox.addItem ("None", 1);

    for (int i = 0; i < devices.size(); ++i)
        deviceBox.addItem (devices[i].name, i + 2);

    const auto openName = surface.getOpenDeviceName();
    auto selected = 1;

    for (int i = 0; i < devices.size(); ++i)
        if (devices[i].name == openName)
            selected = i + 2;

    deviceBox.setSelectedId (selected, juce::dontSendNotification);
}

void MidiSetupComponent::refreshMappingList()
{
    const auto names = surface.getMappingNames();

    mappingBox.clear (juce::dontSendNotification);
    mappingBox.addItem ("None", 1);

    for (int i = 0; i < names.size(); ++i)
        mappingBox.addItem (names[i], i + 2);

    const auto selectedName = surface.getSelectedMappingName();
    auto selected = 1;

    for (int i = 0; i < names.size(); ++i)
        if (names[i] == selectedName)
            selected = i + 2;

    mappingBox.setSelectedId (selected, juce::dontSendNotification);
}

void MidiSetupComponent::timerCallback()
{
    entries = surface.getRecentMessages();
    messageList.updateContent();
    messageList.repaint();

    juce::String status;

    if (! surface.isOpen())
        status = "No controller connected.";
    else
        status = surface.getOpenDeviceName()
               + (surface.hasFeedbackOutput() ? "  |  lights on" : "  |  no output port, lights off");

    statusLabel.setText (status, juce::dontSendNotification);
}

int MidiSetupComponent::getNumRows()
{
    return static_cast<int> (entries.size());
}

void MidiSetupComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (entries.size())))
        return;

    const auto& entry = entries[(size_t) row];
    const auto isMapped = entry.mappedTo != "unmapped";

    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    g.drawText (entry.description, 6, 0, width / 2, height, juce::Justification::centredLeft);

    g.setColour (isMapped ? mappedColour : juce::Colours::dimgrey);
    g.drawText (entry.mappedTo, width / 2, 0, width / 2 - 6, height, juce::Justification::centredLeft);
}

void MidiSetupComponent::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);
}

void MidiSetupComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto row = area.removeFromTop (26);
    deviceLabel.setBounds (row.removeFromLeft (76));
    rescanButton.setBounds (row.removeFromRight (76));
    row.removeFromRight (6);
    deviceBox.setBounds (row);

    area.removeFromTop (6);

    row = area.removeFromTop (26);
    mappingLabel.setBounds (row.removeFromLeft (76));
    clearButton.setBounds (row.removeFromRight (76));
    row.removeFromRight (6);
    mappingBox.setBounds (row);

    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);

    warningsBox.setBounds (area.removeFromBottom (80));
    area.removeFromBottom (6);
    messageList.setBounds (area);
}

} // namespace opendj
