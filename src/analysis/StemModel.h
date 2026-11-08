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

/** Runs htdemucs in this process, through ONNX Runtime or OpenVINO.

    The converted model is only the middle of htdemucs: the transforms at either
    end were stripped out of the graph, so everything around this -- the STFT,
    the normalisation, the splitting into segments and the blending of them back
    together -- lives in StemModel.cpp and in StemDsp. Feeding it anything other
    than exactly what it expects produces plausible nonsense rather than an
    error, which is why the transforms are pinned against a reference.

    There are two engines, because no one runtime reaches every GPU. ONNX
    Runtime puts it on an NVIDIA card through CUDA, or on a Mac through Core ML,
    and needs htdemucs_v4.onnx. OpenVINO reaches Intel graphics and any CPU, and
    needs htdemucs_v4.xml. Either is built in only when it was found; with
    neither this reports itself unavailable and OpenDJ falls back to an
    external separator, or to none.
*/
class StemModel
{
public:
    StemModel();
    ~StemModel();

    enum class Engine
    {
        automatic,      // ONNX Runtime on a GPU where it can, OpenVINO otherwise
        onnxRuntime,
        openVino
    };

    /** Finds and loads the model. Slow, and safe to call from a background
        thread. Returns an error string, empty on success. */
    juce::String load (Engine engine = Engine::automatic);

    bool isLoaded() const;

    /** Which device it is running on, for the interface. */
    juce::String getDeviceName() const;

    /** The weights in use, once loaded. */
    juce::File getModelFile() const;

    /** Why the GPU engine was passed over for OpenVINO, when it was. Loading
        still succeeded, so this is not an error, but it says why stems are
        slower than they could be: a missing cuDNN, say. */
    juce::String getFallbackReason() const;

    /** Where the weights were found, or where they were looked for: the
        OpenVINO model and the ONNX one. */
    static juce::File findModel();
    static juce::File findOnnxModel();
    static juce::StringArray getSearchedLocations();

    /** Separates a whole track. `mix` is two channels of any length at 44100 Hz.
        Progress is a fraction, and returning false from it gives up. */
    bool separate (const juce::AudioBuffer<float>& mix,
                   SeparatedTrack& destination,
                   const std::function<bool (float)>& onProgress = {});

    /** One engine, defined beside the code that drives it. */
    struct Backend;

private:
    std::unique_ptr<Backend> backend;
    juce::String fallbackReason;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemModel)
};

} // namespace opendj
