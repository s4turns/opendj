/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MidiSetupComponent.h"

#include <map>

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

    setUpLabel (learnLabel, "Learn");

    learnStatus.setColour (juce::Label::textColourId, mappedColour);
    learnStatus.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (learnStatus);

    // A list of nearly eighty is only usable in groups, and the first word of
    // each action's name is already the group it belongs to.
    {
        std::map<juce::String, juce::PopupMenu> groups;

        for (int i = 1; i <= (int) lastAction; ++i)
            groups[toString ((Action) i).upToFirstOccurrenceOf (".", false, false)]
                .addItem (i, toString ((Action) i));

        for (auto& [group, menu] : groups)
            actionBox.getRootMenu()->addSubMenu (group, menu);
    }

    actionBox.setTextWhenNothingSelected ("Choose what to learn...");
    actionBox.onChange = [this] { refreshLearnControls(); };
    addAndMakeVisible (actionBox);

    for (int deck = 0; deck < AudioEngine::numDecks; ++deck)
        learnDeckBox.addItem (juce::String::charToString ((juce::juce_wchar) ('A' + deck)), deck + 1);

    learnDeckBox.setSelectedId (1, juce::dontSendNotification);
    learnDeckBox.setTooltip ("Which deck it works on. For a master effect, A is FX 1 and B is FX 2");
    addAndMakeVisible (learnDeckBox);

    for (int slot = 0; slot < 8; ++slot)
        learnSlotBox.addItem (juce::String (slot + 1), slot + 1);

    learnSlotBox.setSelectedId (1, juce::dontSendNotification);
    learnSlotBox.setTooltip ("Which one of several: a hot cue, a pad, a loop length, an EQ band");
    addAndMakeVisible (learnSlotBox);

    learnButton.setTooltip ("Then press or move the control on the hardware. It is saved straight away");
    learnButton.onClick = [this]
    {
        if (surface.isLearning())
            surface.cancelLearning();
        else
            surface.startLearning (chosenTarget());

        refreshLearnControls();
    };
    addAndMakeVisible (learnButton);

    undoButton.setTooltip ("Put the mapping back as it was before the last learn");
    undoButton.onClick = [this]
    {
        surface.undoLearn();
        learnStatus.setText ("Put back as it was", juce::dontSendNotification);
        refreshMappingList();
        refreshLearnControls();
    };
    addAndMakeVisible (undoButton);

    refreshDeviceList();
    refreshMappingList();
    refreshLearnControls();

    const auto warnings = surface.getWarnings();
    warningsBox.setText (warnings.isEmpty() ? "All mappings loaded cleanly."
                                            : warnings.joinIntoString ("\n"),
                         false);

    startTimerHz (10);
    setSize (620, 580);
}

MidiSetupComponent::~MidiSetupComponent()
{
    stopTimer();

    // Going on learning with nobody watching would swallow the next message.
    surface.cancelLearning();
}

LearnTarget MidiSetupComponent::chosenTarget() const
{
    LearnTarget target;
    target.action = (Action) actionBox.getSelectedId();
    target.deck = actionTakesDeck (target.action) ? learnDeckBox.getSelectedId() - 1 : 0;
    target.slot = actionTakesSlot (target.action) ? learnSlotBox.getSelectedId() - 1 : 0;
    return target;
}

void MidiSetupComponent::refreshLearnControls()
{
    const auto learning = surface.isLearning();
    const auto action = (Action) actionBox.getSelectedId();
    const auto chosen = action != Action::none;

    actionBox.setEnabled (! learning);
    learnDeckBox.setEnabled (! learning && chosen && actionTakesDeck (action));
    learnSlotBox.setEnabled (! learning && chosen && actionTakesSlot (action));
    learnButton.setEnabled (learning || (chosen && surface.isOpen()));
    learnButton.setButtonText (learning ? "Cancel" : "Learn");
    undoButton.setEnabled (! learning && surface.canUndoLearn());
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

    if (const auto result = surface.takeLearnResult(); result.isNotEmpty())
    {
        learnStatus.setText (result, juce::dontSendNotification);
        refreshMappingList();
    }
    else if (surface.isLearning())
    {
        learnStatus.setText ("Press or move the control for " + nameForLearnedControl (chosenTarget()) + "...",
                             juce::dontSendNotification);
    }

    refreshLearnControls();

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

    row = area.removeFromTop (26);
    learnLabel.setBounds (row.removeFromLeft (76));
    undoButton.setBounds (row.removeFromRight (56));
    row.removeFromRight (6);
    learnButton.setBounds (row.removeFromRight (70));
    row.removeFromRight (6);
    learnSlotBox.setBounds (row.removeFromRight (52));
    row.removeFromRight (4);
    learnDeckBox.setBounds (row.removeFromRight (52));
    row.removeFromRight (6);
    actionBox.setBounds (row);

    learnStatus.setBounds (area.removeFromTop (20).withTrimmedLeft (76));

    area.removeFromTop (2);
    statusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (6);

    warningsBox.setBounds (area.removeFromBottom (80));
    area.removeFromBottom (6);
    messageList.setBounds (area);
}

} // namespace opendj
