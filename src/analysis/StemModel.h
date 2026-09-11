/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#pragma once

#include "analysis/Stems.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

namespace opendj
{

/** Runs htdemucs in this process, through OpenVINO.

    The converted model is only the middle of htdemucs: the transforms at either
    end were stripped out of the graph, so everything around this -- the STFT,
    the normalisation, the splitting into segments and the blending of them back
    together -- lives in StemModel.cpp and in StemDsp. Feeding it anything other
    than exactly what it expects produces plausible nonsense rather than an
    error, which is why the transforms are pinned against a reference.

    Built only when OpenVINO was found. Without it this reports itself
    unavailable and OpenDJ falls back to an external separator, or to none.
*/
class StemModel
{
public:
    StemModel();
    ~StemModel();

    /** Finds and loads the model. Slow, and safe to call from a background
        thread. Returns an error string, empty on success. */
    juce::String load (const juce::File& modelXml = {});

    bool isLoaded() const;

    /** Which device it is running on, for the interface. */
    juce::String getDeviceName() const;

    /** Where the weights were found, or where they were looked for. */
    static juce::File findModel();
    static juce::StringArray getSearchedLocations();

    /** Separates a whole track. `mix` is two channels of any length at 44100 Hz.
        Progress is a fraction, and returning false from it gives up. */
    bool separate (const juce::AudioBuffer<float>& mix,
                   SeparatedTrack& destination,
                   const std::function<bool (float)>& onProgress = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemModel)
};

} // namespace opendj
