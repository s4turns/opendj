/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "app/Settings.h"

namespace opendj
{

namespace
{
    // Stored as names rather than numbers. A settings file is something a
    // person may well open, and "sharp_cut" says what it is where a 2 does not.
    juce::String curveName (Mixer::CrossfaderCurve curve)
    {
        switch (curve)
        {
            case Mixer::CrossfaderCurve::linear:   return "linear";
            case Mixer::CrossfaderCurve::sharpCut: return "sharp_cut";
            default:                               return "constant_power";
        }
    }

    Mixer::CrossfaderCurve curveFromName (const juce::String& name)
    {
        if (name == "linear")    return Mixer::CrossfaderCurve::linear;
        if (name == "sharp_cut") return Mixer::CrossfaderCurve::sharpCut;

        return Mixer::CrossfaderCurve::constantPower;
    }

    juce::String assignName (Mixer::CrossfaderAssign assign)
    {
        switch (assign)
        {
            case Mixer::CrossfaderAssign::a: return "a";
            case Mixer::CrossfaderAssign::b: return "b";
            default:                         return "thru";
        }
    }

    Mixer::CrossfaderAssign assignFromName (const juce::String& name)
    {
        if (name == "a") return Mixer::CrossfaderAssign::a;
        if (name == "b") return Mixer::CrossfaderAssign::b;

        return Mixer::CrossfaderAssign::thru;
    }

    /** A property, or the default when the file predates it or was hand edited
        into nonsense. Every read goes through one of these, which is what makes
        a half written file harmless. */
    double number (const juce::var& source, const char* key, double fallback,
                   double low, double high)
    {
        if (auto* object = source.getDynamicObject())
            if (const auto value = object->getProperty (key); value.isDouble() || value.isInt())
                return juce::jlimit (low, high, static_cast<double> (value));

        return fallback;
    }

    bool flag (const juce::var& source, const char* key, bool fallback)
    {
        if (auto* object = source.getDynamicObject())
            if (const auto value = object->getProperty (key); value.isBool())
                return static_cast<bool> (value);

        return fallback;
    }

    juce::String text (const juce::var& source, const char* key)
    {
        if (auto* object = source.getDynamicObject())
            if (const auto value = object->getProperty (key); value.isString())
                return value.toString();

        return {};
    }

    juce::var arrayOf (const juce::var& source, const char* key)
    {
        if (auto* object = source.getDynamicObject())
            return object->getProperty (key);

        return {};
    }

    juce::var element (const juce::var& array, int index)
    {
        if (auto* values = array.getArray(); values != nullptr
            && juce::isPositiveAndBelow (index, values->size()))
            return (*values)[index];

        return {};
    }
}

//==============================================================================

void SessionState::captureFrom (const Mixer& mixer, const Sampler& sampler)
{
    masterGain = mixer.getMasterGain();
    cueGain = mixer.getCueGain();
    cueMix = mixer.getCueMix();
    crossfaderCurve = mixer.getCrossfaderCurve();

    for (int c = 0; c < Mixer::numChannels; ++c)
        crossfaderAssigns[(size_t) c] = mixer.getChannelCrossfaderAssign (c);

    samplerGain = sampler.getGain();
    samplerCue = sampler.isCueEnabled();

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        samplerLooping[(size_t) slot] = sampler.isSlotLooping (slot);
        samplerGains[(size_t) slot] = sampler.getSlotGain (slot);
        samplerFiles[(size_t) slot] = sampler.getSlotFile (slot).getFullPathName();
    }
}

void SessionState::applyTo (Mixer& mixer, Sampler& sampler) const
{
    mixer.setMasterGain (masterGain);
    mixer.setCueGain (cueGain);
    mixer.setCueMix (cueMix);
    mixer.setCrossfaderCurve (crossfaderCurve);

    for (int c = 0; c < Mixer::numChannels; ++c)
        mixer.setChannelCrossfaderAssign (c, crossfaderAssigns[(size_t) c]);

    sampler.setGain (samplerGain);
    sampler.setCueEnabled (samplerCue);

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        sampler.setSlotLooping (slot, samplerLooping[(size_t) slot]);
        sampler.setSlotGain (slot, samplerGains[(size_t) slot]);
    }
}

//==============================================================================

juce::var SessionState::toVar() const
{
    auto* root = new juce::DynamicObject();

    root->setProperty ("master_gain", masterGain);
    root->setProperty ("cue_gain", cueGain);
    root->setProperty ("cue_mix", cueMix);
    root->setProperty ("crossfader_curve", curveName (crossfaderCurve));

    juce::Array<juce::var> assigns, ranges, visible, files, loops, gains;

    for (const auto assign : crossfaderAssigns)
        assigns.add (assignName (assign));

    for (const auto range : tempoRanges)
        ranges.add (range);

    for (const auto deck : visibleDecks)
        visible.add (deck);

    for (size_t slot = 0; slot < Sampler::numSlots; ++slot)
    {
        files.add (samplerFiles[slot]);
        loops.add (samplerLooping[slot]);
        gains.add (samplerGains[slot]);
    }

    root->setProperty ("crossfader_assign", assigns);
    root->setProperty ("tempo_ranges", ranges);
    root->setProperty ("visible_decks", visible);

    root->setProperty ("output_mode", opendj::toString (outputMode));
    root->setProperty ("sampler_gain", samplerGain);
    root->setProperty ("sampler_cue", samplerCue);
    root->setProperty ("sampler_files", files);
    root->setProperty ("sampler_looping", loops);
    root->setProperty ("sampler_gains", gains);

    if (windowBounds.isNotEmpty())
        root->setProperty ("window", windowBounds);

    return { root };
}

SessionState SessionState::fromVar (const juce::var& source)
{
    SessionState state;

    state.masterGain = (float) number (source, "master_gain", state.masterGain, 0.0, 1.0);
    state.cueGain = (float) number (source, "cue_gain", state.cueGain, 0.0, 1.0);
    state.cueMix = (float) number (source, "cue_mix", state.cueMix, 0.0, 1.0);
    state.crossfaderCurve = curveFromName (text (source, "crossfader_curve"));

    const auto assigns = arrayOf (source, "crossfader_assign");
    const auto ranges = arrayOf (source, "tempo_ranges");

    for (int c = 0; c < Mixer::numChannels; ++c)
    {
        if (const auto value = element (assigns, c); value.isString())
            state.crossfaderAssigns[(size_t) c] = assignFromName (value.toString());

        if (const auto value = element (ranges, c); value.isDouble() || value.isInt())
            state.tempoRanges[(size_t) c] = juce::jlimit (1.0, 100.0, (double) value);
    }

    const auto visible = arrayOf (source, "visible_decks");

    for (int side = 0; side < 2; ++side)
        if (const auto value = element (visible, side); value.isInt() || value.isDouble())
        {
            const auto deck = (int) value;

            // A side can only show a deck that belongs to it, so a file naming
            // deck C on the right is ignored rather than obeyed.
            if (juce::isPositiveAndBelow (deck, Mixer::numChannels) && deck % 2 == side)
                state.visibleDecks[(size_t) side] = deck;
        }

    state.outputMode = outputModeFromString (text (source, "output_mode"));
    state.samplerGain = (float) number (source, "sampler_gain", state.samplerGain, 0.0, 1.0);
    state.samplerCue = flag (source, "sampler_cue", state.samplerCue);

    const auto files = arrayOf (source, "sampler_files");
    const auto loops = arrayOf (source, "sampler_looping");
    const auto gains = arrayOf (source, "sampler_gains");

    for (int slot = 0; slot < Sampler::numSlots; ++slot)
    {
        if (const auto value = element (files, slot); value.isString())
            state.samplerFiles[(size_t) slot] = value.toString();

        if (const auto value = element (loops, slot); value.isBool())
            state.samplerLooping[(size_t) slot] = (bool) value;

        if (const auto value = element (gains, slot); value.isDouble() || value.isInt())
            state.samplerGains[(size_t) slot] = (float) juce::jlimit (0.0, 2.0, (double) value);
    }

    state.windowBounds = text (source, "window");

    return state;
}

//==============================================================================

juce::File SessionState::defaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("OpenDJ")
             .getChildFile ("settings.json");
}

bool SessionState::writeTo (const juce::File& file) const
{
    if (! file.getParentDirectory().createDirectory())
        return false;

    // Written beside the real file and moved into place, so an interrupted
    // write leaves the previous settings rather than half of the new ones.
    const auto temporary = file.getSiblingFile (file.getFileName() + ".tmp");

    if (! temporary.replaceWithText (juce::JSON::toString (toVar(), false)))
        return false;

    return temporary.moveFileTo (file);
}

SessionState SessionState::readFrom (const juce::File& file)
{
    if (! file.existsAsFile())
        return {};

    return fromVar (juce::JSON::parse (file.loadFileAsString()));
}

} // namespace opendj
