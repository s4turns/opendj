/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "ui/MasterFxComponent.h"

#include <cmath>

namespace opendj
{

namespace
{
    const juce::Colour panelColour  { 0xff1c1c22 };
    const juce::Colour accentColour { 0xff35c2f0 };

    void styleCaption (juce::Label& label)
    {
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::grey);
        label.setFont (juce::FontOptions (11.0f));
        label.setMinimumHorizontalScale (0.6f);
        label.setInterceptsMouseClicks (false, false);
    }

    void configureKnob (juce::Slider& knob)
    {
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange (0.0, 1.0, 0.0);
        knob.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    }

    // The combo box ids are the effect's index plus one, since zero means
    // nothing selected.
    int idFor (MasterEffects::Type type) noexcept { return (int) type + 1; }
}

MasterFxComponent::MasterFxComponent (MasterEffects& effectsToControl)
    : effects (effectsToControl)
{
    for (size_t s = 0; s < units.size(); ++s)
    {
        auto& unit = units[s];
        const auto slot = (int) s;

        unit.heading.setText ("FX " + juce::String (slot + 1), juce::dontSendNotification);
        unit.heading.setJustificationType (juce::Justification::centred);
        unit.heading.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (unit.heading);

        for (int t = 0; t < MasterEffects::numTypes; ++t)
            unit.type.addItem (MasterEffects::getTypeName ((MasterEffects::Type) t), t + 1);

        unit.type.setSelectedId (idFor (effects.getType (slot)), juce::dontSendNotification);
        unit.type.setTooltip ("Which effect this unit runs on the whole mix");
        unit.type.onChange = [this, slot, &unit]
        {
            effects.setType (slot, (MasterEffects::Type) (unit.type.getSelectedId() - 1));
            updateCaptions (slot);
            refresh();
        };
        addAndMakeVisible (unit.type);

        unit.onButton.setClickingTogglesState (true);
        unit.onButton.setColour (juce::TextButton::buttonOnColourId, accentColour.darker (0.3f));
        unit.onButton.setTooltip ("Echo and reverb ring out when switched off; the filter returns to dry");
        unit.onButton.onClick = [this, slot, &unit]
        {
            effects.setEnabled (slot, unit.onButton.getToggleState());
        };
        addAndMakeVisible (unit.onButton);

        configureKnob (unit.wet);
        unit.wet.setDoubleClickReturnValue (true, 0.5);
        unit.wet.setTooltip ("How much of the effect is heard");
        unit.wet.onValueChange = [this, slot, &unit]
        {
            effects.setWet (slot, (float) unit.wet.getValue());
        };
        addAndMakeVisible (unit.wet);

        styleCaption (unit.wetCaption);
        unit.wetCaption.setText ("Wet", juce::dontSendNotification);
        addAndMakeVisible (unit.wetCaption);

        for (int p = 0; p < MasterEffects::numParams; ++p)
        {
            auto& knob = unit.params[(size_t) p];
            configureKnob (knob);
            knob.setDoubleClickReturnValue (true, 0.5);
            knob.onValueChange = [this, slot, p, &knob]
            {
                effects.setParam (slot, p, (float) knob.getValue());
                updateCaptions (slot);
            };
            addAndMakeVisible (knob);

            styleCaption (unit.paramCaptions[(size_t) p]);
            addAndMakeVisible (unit.paramCaptions[(size_t) p]);
        }

        updateCaptions (slot);
    }

    refresh();
}

void MasterFxComponent::updateCaptions (int slot)
{
    auto& unit = units[(size_t) slot];
    const auto type = effects.getType (slot);

    for (int p = 0; p < MasterEffects::numParams; ++p)
    {
        auto text = MasterEffects::getParamName (type, p);

        // The division is the one value worth reading exactly: a knob's angle
        // does not say whether it is on a half beat or a whole one.
        if (type == MasterEffects::Type::echo && p == 0)
            text << " " << MasterEffects::echoDivisionName (
                               MasterEffects::echoDivisionIndexFor (effects.getParam (slot, 0)));

        unit.paramCaptions[(size_t) p].setText (text, juce::dontSendNotification);
        unit.params[(size_t) p].setTooltip (MasterEffects::getTypeName (type) + ": "
                                            + MasterEffects::getParamName (type, p).toLowerCase());
    }
}

void MasterFxComponent::refresh()
{
    const auto follow = [] (juce::Slider& slider, double value)
    {
        if (! slider.isMouseButtonDown() && std::abs (slider.getValue() - value) > 1.0e-4)
            slider.setValue (value, juce::dontSendNotification);
    };

    for (int slot = 0; slot < MasterEffects::numSlots; ++slot)
    {
        auto& unit = units[(size_t) slot];

        if (const auto id = idFor (effects.getType (slot)); unit.type.getSelectedId() != id)
        {
            unit.type.setSelectedId (id, juce::dontSendNotification);
            updateCaptions (slot);
        }

        unit.onButton.setToggleState (effects.isEnabled (slot), juce::dontSendNotification);
        follow (unit.wet, effects.getWet (slot));

        for (int p = 0; p < MasterEffects::numParams; ++p)
        {
            const auto before = unit.params[(size_t) p].getValue();
            follow (unit.params[(size_t) p], effects.getParam (slot, p));

            if (unit.params[(size_t) p].getValue() != before)
                updateCaptions (slot);
        }
    }
}

void MasterFxComponent::paint (juce::Graphics& g)
{
    const auto width = (getWidth() - 8) / (int) units.size();

    for (int i = 0; i < (int) units.size(); ++i)
    {
        g.setColour (panelColour);
        g.fillRoundedRectangle (juce::Rectangle<int> (i * (width + 8), 0, width, getHeight()).toFloat(), 6.0f);
    }
}

void MasterFxComponent::layOutUnit (Unit& unit, juce::Rectangle<int> area)
{
    area = area.reduced (6, 3);

    unit.heading.setBounds (area.removeFromLeft (36));

    auto controls = area.removeFromLeft (juce::jmin (110, area.getWidth() / 3));
    const auto half = controls.getHeight() / 2;
    unit.type.setBounds (controls.removeFromTop (half).reduced (2, 2));
    unit.onButton.setBounds (controls.reduced (2, 2));

    area.removeFromLeft (4);

    const auto knobWidth = area.getWidth() / 3;

    const auto placeKnob = [&area, knobWidth] (juce::Slider& knob, juce::Label& caption)
    {
        auto cell = area.removeFromLeft (knobWidth);
        caption.setBounds (cell.removeFromBottom (13));
        knob.setBounds (cell);
    };

    placeKnob (unit.wet, unit.wetCaption);

    for (int p = 0; p < MasterEffects::numParams; ++p)
        placeKnob (unit.params[(size_t) p], unit.paramCaptions[(size_t) p]);
}

void MasterFxComponent::resized()
{
    auto area = getLocalBounds();
    const auto width = (area.getWidth() - 8) / (int) units.size();

    for (auto& unit : units)
    {
        layOutUnit (unit, area.removeFromLeft (width));
        area.removeFromLeft (8);
    }
}

} // namespace opendj
