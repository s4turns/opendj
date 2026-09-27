/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/MasterEffects.h"

#include <array>

namespace opendj
{

/** The two effect units on the whole mix, side by side: which effect, on or
    off, how much of it, and its two parameters. */
class MasterFxComponent final : public juce::Component
{
public:
    explicit MasterFxComponent (MasterEffects& effectsToControl);

    /** Follows the engine, so a controller moves what is on screen. */
    void refresh();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Unit
    {
        juce::Label heading;
        juce::ComboBox type;
        juce::TextButton onButton { "On" };
        juce::Slider wet;
        std::array<juce::Slider, MasterEffects::numParams> params;
        juce::Label wetCaption;
        std::array<juce::Label, MasterEffects::numParams> paramCaptions;
    };

    void layOutUnit (Unit& unit, juce::Rectangle<int> area);

    /** Names the parameter knobs after what they do for the chosen effect. */
    void updateCaptions (int slot);

    MasterEffects& effects;
    std::array<Unit, MasterEffects::numSlots> units;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterFxComponent)
};

} // namespace opendj
