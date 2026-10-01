/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/MasterEffects.h"
#include "core/PluginHost.h"

#include <array>
#include <functional>
#include <memory>

namespace opendj
{

/** The two effect units on the whole mix, side by side: which effect, on or
    off, how much of it, and its two parameters. A unit holding a VST3 plugin
    also has a Plugin button, for the plugin's own window, for choosing which
    of its parameters the knobs drive, and for changing the plugin. */
class MasterFxComponent final : public juce::Component
{
public:
    MasterFxComponent (MasterEffects& effectsToControl, PluginHost& hostToUse);
    ~MasterFxComponent() override;

    /** Follows the engine, so a controller moves what is on screen. */
    void refresh();

    /** Loads a plugin saved in the settings into a slot, with its state.
        Returns false, having said why through onMessage, when it cannot. */
    bool restorePlugin (int slot, const juce::String& identifier, const juce::String& name,
                        const juce::String& base64State);

    /** Something worth a line in the status bar. */
    std::function<void (const juce::String&)> onMessage;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Unit
    {
        juce::Label heading;
        juce::ComboBox type;
        juce::TextButton onButton { "On" };
        juce::TextButton pluginButton { "Plugin" };
        juce::Slider wet;
        std::array<juce::Slider, MasterEffects::numParams> params;
        juce::Label wetCaption;
        std::array<juce::Label, MasterEffects::numParams> paramCaptions;
        std::unique_ptr<juce::DocumentWindow> editorWindow;
        int previousTypeId = 1;
    };

    void layOutUnit (Unit& unit, juce::Rectangle<int> area);

    /** Names the parameter knobs after what they do for the chosen effect. */
    void updateCaptions (int slot);

    void showPluginMenu (int slot);
    void loadPlugin (int slot, const juce::PluginDescription& description);
    void removePlugin (int slot);
    void openEditor (int slot);
    void closeEditor (int slot);
    void scanForPlugins();
    void say (const juce::String& message);

    MasterEffects& effects;
    PluginHost& host;
    std::array<Unit, MasterEffects::numSlots> units;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterFxComponent)
};

} // namespace opendj
