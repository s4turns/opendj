/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/SamplerComponent.h"

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour padColour    { 0xff262630 };
    const juce::Colour accentColour { 0xff35c2f0 };

    constexpr int padGap = 4;
}

//==============================================================================

void SamplerComponent::Pad::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    const auto fill = playing ? accentColour.withAlpha (0.85f)
                    : loaded  ? padColour.brighter (0.25f)
                              : padColour;

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (loaded ? accentColour.withAlpha (0.6f) : juce::Colours::white.withAlpha (0.15f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);

    g.setColour (playing ? juce::Colours::black : juce::Colours::white.withAlpha (0.5f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (juce::String (number), bounds.reduced (5.0f, 3.0f),
                juce::Justification::topLeft, false);

    if (looping)
        g.drawText ("loop", bounds.reduced (5.0f, 3.0f), juce::Justification::topRight, false);

    g.setColour (playing ? juce::Colours::black : juce::Colours::white.withAlpha (0.8f));
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (loaded ? name : "empty", getLocalBounds().reduced (5, 14),
                      juce::Justification::centredBottom, 2, 0.8f);
}

//==============================================================================

SamplerComponent::SamplerComponent (AudioEngine& engineToUse) : engine (engineToUse)
{
    heading.setText ("Sampler", juce::dontSendNotification);
    heading.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    heading.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (heading);

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        auto& pad = pads[(size_t) slot];
        pad.number = slot + 1;
        pad.onClick = [this, slot] (const juce::MouseEvent& e) { padClicked (slot, e); };
        addAndMakeVisible (pad);
    }

    level.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    level.setRange (0.0, 1.0, 0.001);
    level.setValue (engine.getSampler().getGain(), juce::dontSendNotification);
    level.setDoubleClickReturnValue (true, 0.8);
    level.setTooltip ("The level of the whole sampler");
    level.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    level.onValueChange = [this] { engine.getSampler().setGain ((float) level.getValue()); };
    addAndMakeVisible (level);

    levelLabel.setText ("Level", juce::dontSendNotification);
    levelLabel.setJustificationType (juce::Justification::centred);
    levelLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    levelLabel.setFont (juce::FontOptions (11.0f));
    levelLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (levelLabel);

    cueButton.setClickingTogglesState (true);
    cueButton.setTooltip ("Send the sampler to the headphones as well as the room");
    cueButton.setColour (juce::TextButton::buttonOnColourId, accentColour);
    cueButton.onClick = [this]
    {
        engine.getSampler().setCueEnabled (cueButton.getToggleState());
    };
    addAndMakeVisible (cueButton);

    startTimerHz (20);
}

SamplerComponent::~SamplerComponent() { stopTimer(); }

void SamplerComponent::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
}

void SamplerComponent::resized()
{
    auto area = getLocalBounds().reduced (6, 4);

    heading.setBounds (area.removeFromLeft (56));

    auto right = area.removeFromRight (96);
    cueButton.setBounds (right.removeFromRight (44).reduced (2, 6));
    auto knobCell = right.reduced (2);
    levelLabel.setBounds (knobCell.removeFromBottom (12));
    level.setBounds (knobCell);

    // The pads share whatever is left, so the row narrows with the window
    // rather than the last pad falling off the end of it.
    const auto padWidth = (area.getWidth() - padGap * (Sampler::numSlots - 1)) / Sampler::numSlots;

    for (auto& pad : pads)
    {
        pad.setBounds (area.removeFromLeft (padWidth));
        area.removeFromLeft (padGap);
    }
}

//==============================================================================

void SamplerComponent::timerCallback()
{
    auto& sampler = engine.getSampler();

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        auto& pad = pads[(size_t) slot];

        const auto loaded = sampler.isSlotLoaded (slot);
        const auto playing = sampler.isSlotPlaying (slot);
        const auto looping = sampler.isSlotLooping (slot);
        const auto name = sampler.getSlotName (slot);

        if (loaded != pad.loaded || playing != pad.playing
            || looping != pad.looping || name != pad.name)
        {
            pad.loaded = loaded;
            pad.playing = playing;
            pad.looping = looping;
            pad.name = name;
            pad.repaint();
        }
    }
}

//==============================================================================

void SamplerComponent::padClicked (int slot, const juce::MouseEvent& e)
{
    auto& sampler = engine.getSampler();

    if (e.mods.isPopupMenu())
    {
        showPadMenu (slot);
        return;
    }

    if (! sampler.isSlotLoaded (slot))
    {
        chooseFileFor (slot);
        return;
    }

    // Shift stops, which is what the pads on hardware do and what a looping
    // slot needs from a row of buttons that has no second row.
    if (e.mods.isShiftDown())
        sampler.stop (slot);
    else
        sampler.trigger (slot);

    pads[(size_t) slot].repaint();
}

void SamplerComponent::showPadMenu (int slot)
{
    auto& sampler = engine.getSampler();
    const auto loaded = sampler.isSlotLoaded (slot);

    juce::PopupMenu menu;
    menu.addItem (1, loaded ? "Replace sound..." : "Load sound...");
    menu.addItem (2, "Loop", loaded, sampler.isSlotLooping (slot));
    menu.addItem (3, "Stop", loaded && sampler.isSlotPlaying (slot));
    menu.addSeparator();
    menu.addItem (4, "Clear", loaded);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pads[(size_t) slot]),
                        [this, slot] (int result)
    {
        auto& s = engine.getSampler();

        switch (result)
        {
            case 1: chooseFileFor (slot); break;
            case 2: s.setSlotLooping (slot, ! s.isSlotLooping (slot)); break;
            case 3: s.stop (slot); break;
            case 4: s.clearSlot (slot); break;
            default: break;
        }

        pads[(size_t) slot].repaint();
    });
}

void SamplerComponent::chooseFileFor (int slot)
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sound into pad " + juce::String (slot + 1),
                                                   juce::File(),
                                                   "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                          [this, slot] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file.existsAsFile())
            loadInto (slot, file);
    });
}

void SamplerComponent::loadInto (int slot, const juce::File& file)
{
    engine.loadSampleAsync (slot, file, [this, slot, file] (juce::String error)
    {
        if (onMessage != nullptr)
            onMessage (error.isEmpty() ? "Pad " + juce::String (slot + 1) + ": " + file.getFileName()
                                       : error);

        pads[(size_t) slot].repaint();
    });
}

//==============================================================================

int SamplerComponent::padAt (juce::Point<int> position) const
{
    for (int slot = 0; slot < Sampler::numSlots; ++slot)
        if (pads[(size_t) slot].getBounds().contains (position))
            return slot;

    return -1;
}

bool SamplerComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const auto extension = juce::File (path).getFileExtension().toLowerCase();

        if (extension == ".wav" || extension == ".aiff" || extension == ".aif"
            || extension == ".flac" || extension == ".mp3" || extension == ".ogg")
            return true;
    }

    return false;
}

void SamplerComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (files.isEmpty())
        return;

    auto slot = padAt ({ x, y });

    // A file dropped between the pads, or on the heading, fills the first free
    // one rather than being thrown away.
    if (slot < 0)
    {
        for (int i = 0; i < Sampler::numSlots && slot < 0; ++i)
            if (! engine.getSampler().isSlotLoaded (i))
                slot = i;
    }

    if (slot >= 0)
        loadInto (slot, juce::File (files[0]));
}

} // namespace opendj
