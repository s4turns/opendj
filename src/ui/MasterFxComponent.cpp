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

    const int pluginId = idFor (MasterEffects::Type::plugin);

    /** A plugin's own window. Closing it only hides it from view; the unit
        that owns it decides when it goes, which is always before its plugin. */
    class PluginWindow final : public juce::DocumentWindow
    {
    public:
        PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> onClose)
            : DocumentWindow (title, juce::Colour (0xff1c1c22), DocumentWindow::closeButton),
              closed (std::move (onClose))
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            if (closed != nullptr)
                closed();
        }

    private:
        std::function<void()> closed;
    };
}

MasterFxComponent::MasterFxComponent (MasterEffects& effectsToControl, PluginHost& hostToUse)
    : effects (effectsToControl), host (hostToUse)
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
        unit.previousTypeId = unit.type.getSelectedId();
        unit.type.setTooltip ("Which effect this unit runs on the whole mix");
        unit.type.onChange = [this, slot, &unit]
        {
            const auto id = unit.type.getSelectedId();

            // Choosing Plugin with nothing loaded means choosing a plugin.
            // The menu puts the box back if nothing is picked.
            if (id == pluginId && effects.getPlugin (slot) == nullptr)
            {
                showPluginMenu (slot);
                return;
            }

            unit.previousTypeId = id;
            effects.setType (slot, (MasterEffects::Type) (id - 1));
            updateCaptions (slot);
            refresh();
        };
        addAndMakeVisible (unit.type);

        unit.onButton.setClickingTogglesState (true);
        unit.onButton.setColour (juce::TextButton::buttonOnColourId, accentColour.darker (0.3f));
        unit.onButton.setTooltip ("Echo and reverb ring out when switched off; the filter and plugins return to dry");
        unit.onButton.onClick = [this, slot, &unit]
        {
            effects.setEnabled (slot, unit.onButton.getToggleState());
        };
        addAndMakeVisible (unit.onButton);

        unit.pluginButton.setTooltip ("The plugin's own window, which parameters the knobs drive, and which plugin");
        unit.pluginButton.onClick = [this, slot] { showPluginMenu (slot); };
        addChildComponent (unit.pluginButton);

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

MasterFxComponent::~MasterFxComponent()
{
    // Editors go before anything else, while their plugins are certainly alive.
    for (int slot = 0; slot < MasterEffects::numSlots; ++slot)
        closeEditor (slot);
}

void MasterFxComponent::say (const juce::String& message)
{
    if (onMessage != nullptr)
        onMessage (message);
}

void MasterFxComponent::updateCaptions (int slot)
{
    auto& unit = units[(size_t) slot];
    const auto type = effects.getType (slot);

    for (int p = 0; p < MasterEffects::numParams; ++p)
    {
        auto text = effects.getParamLabel (slot, p);

        // The division is the one value worth reading exactly: a knob's angle
        // does not say whether it is on a half beat or a whole one.
        if (type == MasterEffects::Type::echo && p == 0)
            text << " " << MasterEffects::echoDivisionName (
                               MasterEffects::echoDivisionIndexFor (effects.getParam (slot, 0)));

        unit.paramCaptions[(size_t) p].setText (text, juce::dontSendNotification);
        unit.params[(size_t) p].setTooltip (MasterEffects::getTypeName (type) + ": " + text);
    }

    // The menu item carries the plugin's name, so the box says what is loaded.
    auto* plugin = effects.getPlugin (slot);
    unit.type.changeItemText (pluginId, plugin != nullptr ? plugin->getName() : juce::String ("Plugin..."));

    const auto showPlugin = type == MasterEffects::Type::plugin;

    if (unit.pluginButton.isVisible() != showPlugin)
    {
        unit.pluginButton.setVisible (showPlugin);
        resized();
    }

    if (plugin != nullptr)
    {
        auto tip = plugin->getName();

        if (const auto latency = plugin->getLatencySamples(); latency > 0 && plugin->getSampleRate() > 0.0)
            tip << ", adding " << juce::String (1000.0 * latency / plugin->getSampleRate(), 1) << " ms of delay to the mix";

        unit.pluginButton.setTooltip (tip);
    }
}

void MasterFxComponent::showPluginMenu (int slot)
{
    auto* plugin = effects.getPlugin (slot);
    juce::PopupMenu menu;

    constexpr int openEditorId = 1, scanId = 2, removeId = 3;
    constexpr int knobBase = 1000;     // + knob * 10000 + parameter
    constexpr int pluginBase = 100000; // + index into the effect list

    if (plugin != nullptr)
    {
        menu.addSectionHeader (plugin->getName());
        menu.addItem (openEditorId, "Open its window");

        const auto& parameters = plugin->getParameters();

        for (int knob = 0; knob < MasterEffects::numParams; ++knob)
        {
            juce::PopupMenu choices;
            const auto current = effects.getPluginParamIndex (slot, knob);

            // Long lists are cut at a length a menu can show; a plugin with
            // hundreds of parameters puts the useful ones first.
            for (int i = 0; i < juce::jmin (parameters.size(), 200); ++i)
                choices.addItem (knobBase + knob * 10000 + i, parameters[i]->getName (40), true, i == current);

            menu.addSubMenu ("Knob " + juce::String (knob + 1) + " drives", choices, ! parameters.isEmpty());
        }

        menu.addSeparator();
    }

    const auto effectsFound = host.getEffects();
    juce::PopupMenu choose;
    juce::String maker;
    juce::PopupMenu byMaker;

    const auto flush = [&]
    {
        if (maker.isNotEmpty() || byMaker.getNumItems() > 0)
            choose.addSubMenu (maker.isNotEmpty() ? maker : juce::String ("Other"), byMaker);

        byMaker = {};
    };

    for (int i = 0; i < effectsFound.size(); ++i)
    {
        if (effectsFound[i].manufacturerName != maker)
        {
            flush();
            maker = effectsFound[i].manufacturerName;
        }

        byMaker.addItem (pluginBase + i, effectsFound[i].name);
    }

    flush();

    if (effectsFound.isEmpty())
        menu.addItem (-1, host.isScanning() ? "Scanning for plugins..." : "No plugins found yet", false);
    else
        menu.addSubMenu (plugin != nullptr ? "Change plugin" : "Choose a plugin", choose);

    menu.addItem (scanId, "Scan for plugins", ! host.isScanning());

    if (plugin != nullptr)
        menu.addItem (removeId, "Remove plugin");

    auto& unit = units[(size_t) slot];
    auto* target = unit.pluginButton.isVisible() ? static_cast<juce::Component*> (&unit.pluginButton)
                                                 : static_cast<juce::Component*> (&unit.type);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                        [safe = juce::Component::SafePointer<MasterFxComponent> (this), slot, effectsFound] (int result)
    {
        if (safe == nullptr)
            return;

        auto& self = *safe;
        auto& u = self.units[(size_t) slot];

        if (result == openEditorId)
            self.openEditor (slot);
        else if (result == scanId)
            self.scanForPlugins();
        else if (result == removeId)
            self.removePlugin (slot);
        else if (result >= pluginBase && result - pluginBase < effectsFound.size())
            self.loadPlugin (slot, effectsFound[result - pluginBase]);
        else if (result >= knobBase && result < pluginBase)
        {
            const auto knob = (result - knobBase) / 10000;
            self.effects.setPluginParamIndex (slot, knob, (result - knobBase) % 10000);
            self.updateCaptions (slot);
            self.refresh();
        }

        // Nothing loaded after all: the box goes back to what it said before.
        if (self.effects.getPlugin (slot) == nullptr && u.type.getSelectedId() == pluginId)
            u.type.setSelectedId (u.previousTypeId, juce::dontSendNotification);
    });
}

void MasterFxComponent::loadPlugin (int slot, const juce::PluginDescription& description)
{
    juce::String error;
    auto instance = host.createInstance (description, 48000.0, 512, error);

    if (instance == nullptr)
    {
        say ("Could not load " + description.name + ": " + error);
        return;
    }

    closeEditor (slot);

    if (! effects.setPlugin (slot, std::move (instance)))
    {
        say (description.name + " wants more than two channels, which a master effect cannot give it");
        return;
    }

    // A new plugin starts with its first two parameters on the knobs.
    for (int p = 0; p < MasterEffects::numParams; ++p)
        effects.setPluginParamIndex (slot, p, p);

    auto& unit = units[(size_t) slot];
    effects.setType (slot, MasterEffects::Type::plugin);
    unit.previousTypeId = pluginId;
    updateCaptions (slot);
    refresh();
    say (description.name + " is in FX " + juce::String (slot + 1));
}

bool MasterFxComponent::restorePlugin (int slot, const juce::String& identifier, const juce::String& name,
                                       const juce::String& base64State)
{
    if (identifier.isEmpty() || ! juce::isPositiveAndBelow (slot, MasterEffects::numSlots))
        return false;

    // The marker stays behind only if loading this kills the process.
    host.beginRiskyLoad (name);

    juce::String error;
    auto instance = host.createInstance (identifier, 48000.0, 512, error);

    if (instance != nullptr && base64State.isNotEmpty())
    {
        juce::MemoryBlock state;

        if (state.fromBase64Encoding (base64State))
            instance->setStateInformation (state.getData(), (int) state.getSize());
    }

    host.endRiskyLoad();

    if (instance == nullptr)
    {
        say ("FX " + juce::String (slot + 1) + " held " + (name.isNotEmpty() ? name : juce::String ("a plugin"))
             + ", which could not be loaded: " + error);
        return false;
    }

    if (! effects.setPlugin (slot, std::move (instance)))
        return false;

    updateCaptions (slot);
    refresh();
    return true;
}

void MasterFxComponent::removePlugin (int slot)
{
    closeEditor (slot);
    effects.setPlugin (slot, nullptr);

    auto& unit = units[(size_t) slot];

    if (effects.getType (slot) == MasterEffects::Type::plugin)
    {
        effects.setType (slot, MasterEffects::Type::echo);
        unit.previousTypeId = idFor (MasterEffects::Type::echo);
    }

    updateCaptions (slot);
    refresh();
}

void MasterFxComponent::openEditor (int slot)
{
    auto& unit = units[(size_t) slot];
    auto* plugin = effects.getPlugin (slot);

    if (plugin == nullptr)
        return;

    if (unit.editorWindow != nullptr)
    {
        unit.editorWindow->toFront (true);
        return;
    }

    auto* editor = plugin->createEditorIfNeeded();

    // Some plugins have no window of their own; a plain list of sliders will
    // do for those.
    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*plugin);

    unit.editorWindow = std::make_unique<PluginWindow> (
        plugin->getName() + " - FX " + juce::String (slot + 1), editor,
        [safe = juce::Component::SafePointer<MasterFxComponent> (this), slot]
        {
            if (safe != nullptr)
                juce::MessageManager::callAsync ([safe, slot]
                {
                    if (safe != nullptr)
                        safe->closeEditor (slot);
                });
        });
}

void MasterFxComponent::closeEditor (int slot)
{
    // The window owns the editor, and the editor tells its plugin as it goes.
    units[(size_t) slot].editorWindow.reset();
}

void MasterFxComponent::scanForPlugins()
{
    say ("Scanning for VST3 plugins...");

    host.scanAsync ([safe = juce::Component::SafePointer<MasterFxComponent> (this)] (int count, juce::StringArray failed)
    {
        if (safe == nullptr)
            return;

        auto message = juce::String (count) + (count == 1 ? " plugin" : " plugins") + " found";

        if (! failed.isEmpty())
            message << ", " << failed.size() << " left out after failing to load";

        safe->say (message);
    });
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
            unit.previousTypeId = id;
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

    auto controls = area.removeFromLeft (juce::jmin (130, area.getWidth() / 3));
    const auto half = controls.getHeight() / 2;
    unit.type.setBounds (controls.removeFromTop (half).reduced (2, 2));

    // With a plugin loaded, the bottom row shares room with its button.
    if (unit.pluginButton.isVisible())
        unit.pluginButton.setBounds (controls.removeFromRight (controls.getWidth() / 2).reduced (2, 2));

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
