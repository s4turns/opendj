/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemModel.h"

#include "analysis/StemDsp.h"

#include <cmath>
#include <numeric>

#if OPENDJ_HAVE_OPENVINO
 #include <openvino/openvino.hpp>
#endif

namespace opendj
{

namespace dsp = stemdsp;

namespace
{
    constexpr float epsilon = 1.0e-5f;

    /** demucs normalises by the sample standard deviation, the one with N-1 on
        the bottom. Using the population figure instead shifts every input to
        the model by a hair, which is the kind of mistake that produces output
        that sounds almost right. */
    struct MeanAndDeviation { float mean, deviation; };

    MeanAndDeviation meanAndDeviation (const float* values, size_t count)
    {
        if (count == 0)
            return { 0.0f, 0.0f };

        double sum = 0.0;

        for (size_t i = 0; i < count; ++i)
            sum += values[i];

        const auto mean = sum / (double) count;

        if (count < 2)
            return { (float) mean, 0.0f };

        double squares = 0.0;

        for (size_t i = 0; i < count; ++i)
        {
            const auto d = values[i] - mean;
            squares += d * d;
        }

        return { (float) mean, (float) std::sqrt (squares / (double) (count - 1)) };
    }

    /** The triangular window segments are blended with, so that the seam
        between two of them is never heard. */
    std::vector<float> transitionWeight()
    {
        std::vector<float> weight ((size_t) dsp::segment);
        const auto half = dsp::segment / 2;

        for (int i = 0; i < dsp::segment; ++i)
            weight[(size_t) i] = (float) (i < half ? i + 1 : dsp::segment - i);

        const auto peak = *std::max_element (weight.begin(), weight.end());

        for (auto& w : weight)
            w /= peak;

        return weight;
    }
}

#if OPENDJ_HAVE_OPENVINO

struct StemModel::Impl
{
    ov::Core core;
    ov::CompiledModel compiled;
    ov::InferRequest request;

    juce::String device;
    bool loaded = false;

    // Which output is which, decided by shape rather than by name: the names in
    // the converted graph are numbers that change with the conversion.
    size_t spectrumOutput = 0;
    size_t waveformOutput = 1;
};

StemModel::StemModel() : impl (std::make_unique<Impl>()) {}
StemModel::~StemModel() = default;

bool StemModel::isLoaded() const              { return impl != nullptr && impl->loaded; }
juce::String StemModel::getDeviceName() const { return impl != nullptr ? impl->device : juce::String(); }

juce::String StemModel::load (const juce::File& modelXml)
{
    const auto file = modelXml.existsAsFile() ? modelXml : findModel();

    if (! file.existsAsFile())
        return "No htdemucs model found. Looked in: " + getSearchedLocations().joinIntoString (", ");

    try
    {
        auto model = impl->core.read_model (file.getFullPathName().toStdString());

        // The GPU is several times faster where there is one, but a machine
        // without a usable one must still be able to separate, slowly.
        const auto devices = impl->core.get_available_devices();
        const auto hasGpu = std::find (devices.begin(), devices.end(), "GPU") != devices.end();

        impl->device = hasGpu ? "GPU" : "CPU";

        try
        {
            impl->compiled = impl->core.compile_model (model, impl->device.toStdString());
        }
        catch (const std::exception&)
        {
            impl->device = "CPU";
            impl->compiled = impl->core.compile_model (model, "CPU");
        }

        impl->request = impl->compiled.create_infer_request();

        const auto outputs = impl->compiled.outputs();

        if (outputs.size() != 2)
            return "The model has " + juce::String ((int) outputs.size()) + " outputs, expected 2.";

        // The spectrum output is four dimensional, the waveform three.
        impl->spectrumOutput = outputs[0].get_shape().size() == 4 ? 0 : 1;
        impl->waveformOutput = 1 - impl->spectrumOutput;

        impl->loaded = true;
        return {};
    }
    catch (const std::exception& e)
    {
        impl->loaded = false;
        return juce::String ("Could not load the model: ") + e.what();
    }
}

bool StemModel::separate (const juce::AudioBuffer<float>& mix,
                          SeparatedTrack& destination,
                          const std::function<bool (float)>& onProgress)
{
    if (! isLoaded() || mix.getNumChannels() < 2 || mix.getNumSamples() <= 0)
        return false;

    const auto length = mix.getNumSamples();

    // demucs normalises the whole track against its own mono average first.
    std::vector<float> reference ((size_t) length);

    for (int i = 0; i < length; ++i)
        reference[(size_t) i] = 0.5f * (mix.getSample (0, i) + mix.getSample (1, i));

    const auto stats = meanAndDeviation (reference.data(), reference.size());

    destination.sampleRate = dsp::sampleRate;

    for (auto& stem : destination.stems)
    {
        stem.setSize (2, length);
        stem.clear();
    }

    if (stats.deviation < 1.0e-8f)
        return true;                    // digital silence separates into silence

    // Normalised copy of the track, which is what the segments are cut from.
    std::vector<std::vector<float>> normalised (2, std::vector<float> ((size_t) length));

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < length; ++i)
            normalised[(size_t) ch][(size_t) i] = (mix.getSample (ch, i) - stats.mean) / stats.deviation;

    const auto weight = transitionWeight();
    const auto stride = (int) (0.75 * dsp::segment);
    const auto numSegments = (length + stride - 1) / stride;

    std::vector<float> weightSum ((size_t) length, 0.0f);

    std::vector<float> specInput ((size_t) (4 * dsp::freqBins * dsp::frames));
    std::vector<float> timeInput ((size_t) (2 * dsp::segment));
    std::vector<float> chunk ((size_t) (2 * dsp::segment));

    for (int s = 0; s < numSegments; ++s)
    {
        const auto offset = s * stride;
        const auto chunkLength = juce::jmin (dsp::segment, length - offset);
        const auto delta = dsp::segment - chunkLength;
        const auto start = offset - delta / 2;

        // The chunk is padded to the model's length with real neighbouring
        // audio where there is any, and with silence past the ends.
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* out = chunk.data() + (size_t) (ch * dsp::segment);

            for (int i = 0; i < dsp::segment; ++i)
            {
                const auto index = start + i;
                out[i] = index >= 0 && index < length ? normalised[(size_t) ch][(size_t) index] : 0.0f;
            }
        }

        // Everything the conversion stripped from HTDemucs.forward happens here.
        std::array<dsp::Spectrogram, 2> spectra;

        for (int ch = 0; ch < 2; ++ch)
            spectra[(size_t) ch] = dsp::spec (chunk.data() + (size_t) (ch * dsp::segment), dsp::segment);

        // Complex-as-channels: real and imaginary become separate planes.
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int bin = 0; bin < dsp::freqBins; ++bin)
            {
                for (int f = 0; f < dsp::frames; ++f)
                {
                    const auto at = (size_t) ((bin * dsp::frames) + f);
                    specInput[(size_t) ((ch * 2) * dsp::freqBins * dsp::frames) + at] = spectra[(size_t) ch].re (bin, f);
                    specInput[(size_t) ((ch * 2 + 1) * dsp::freqBins * dsp::frames) + at] = spectra[(size_t) ch].im (bin, f);
                }
            }
        }

        const auto specStats = meanAndDeviation (specInput.data(), specInput.size());

        for (auto& v : specInput)
            v = (v - specStats.mean) / (epsilon + specStats.deviation);

        std::copy (chunk.begin(), chunk.end(), timeInput.begin());
        const auto timeStats = meanAndDeviation (timeInput.data(), timeInput.size());

        for (auto& v : timeInput)
            v = (v - timeStats.mean) / (epsilon + timeStats.deviation);

        try
        {
            impl->request.set_input_tensor (0, ov::Tensor (ov::element::f32,
                                                           { 1, 4, (size_t) dsp::freqBins, (size_t) dsp::frames },
                                                           specInput.data()));
            impl->request.set_input_tensor (1, ov::Tensor (ov::element::f32,
                                                           { 1, 2, (size_t) dsp::segment },
                                                           timeInput.data()));
            impl->request.infer();
        }
        catch (const std::exception&)
        {
            return false;
        }

        const auto specOut = impl->request.get_output_tensor (impl->spectrumOutput);
        const auto timeOut = impl->request.get_output_tensor (impl->waveformOutput);
        const auto* specData = specOut.data<const float>();
        const auto* timeData = timeOut.data<const float>();

        const auto planeSize = (size_t) (dsp::freqBins * dsp::frames);

        for (int source = 0; source < numStems; ++source)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                // Undo the normalisation, then come back to a waveform.
                dsp::Spectrogram z;
                z.resize (dsp::freqBins, dsp::frames);

                const auto realPlane = (size_t) (source * 4 + ch * 2) * planeSize;
                const auto imagPlane = (size_t) (source * 4 + ch * 2 + 1) * planeSize;

                for (int bin = 0; bin < dsp::freqBins; ++bin)
                {
                    for (int f = 0; f < dsp::frames; ++f)
                    {
                        const auto at = (size_t) (bin * dsp::frames + f);
                        z.re (bin, f) = specData[realPlane + at] * specStats.deviation + specStats.mean;
                        z.im (bin, f) = specData[imagPlane + at] * specStats.deviation + specStats.mean;
                    }
                }

                const auto wave = dsp::ispec (z, dsp::segment);
                const auto* waveform = timeData + (size_t) (source * 2 + ch) * (size_t) dsp::segment;

                // The two halves of the model are summed, then the segment is
                // blended into the track with its triangular weight.
                auto* target = destination.stems[(size_t) source].getWritePointer (ch);

                for (int i = 0; i < chunkLength; ++i)
                {
                    const auto fromSegment = delta / 2 + i;
                    const auto value = wave[(size_t) fromSegment]
                                     + waveform[fromSegment] * timeStats.deviation + timeStats.mean;

                    target[offset + i] += value * weight[(size_t) i] * stats.deviation;
                }
            }
        }

        for (int i = 0; i < chunkLength; ++i)
            weightSum[(size_t) (offset + i)] += weight[(size_t) i];

        if (onProgress && ! onProgress ((float) (s + 1) / (float) numSegments))
            return false;
    }

    // Divide out the blending weights and put the track's own level back.
    for (auto& stem : destination.stems)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* samples = stem.getWritePointer (ch);

            for (int i = 0; i < length; ++i)
            {
                const auto w = weightSum[(size_t) i];
                samples[i] = w > 0.0f ? samples[i] / w + stats.mean : 0.0f;
            }
        }
    }

    return true;
}

#else // no OpenVINO

struct StemModel::Impl {};

StemModel::StemModel() = default;
StemModel::~StemModel() = default;
bool StemModel::isLoaded() const { return false; }
juce::String StemModel::getDeviceName() const { return {}; }

juce::String StemModel::load (const juce::File&)
{
    return "OpenDJ was built without OpenVINO, so it cannot separate stems itself.";
}

bool StemModel::separate (const juce::AudioBuffer<float>&, SeparatedTrack&,
                          const std::function<bool (float)>&)
{
    return false;
}

#endif

juce::StringArray StemModel::getSearchedLocations()
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    return
    {
        "$OPENDJ_STEM_MODEL",
        home.getChildFile (".local/share/opendj/models").getFullPathName(),
        home.getChildFile (".local/share/stemsep/models").getFullPathName(),
        "/usr/local/lib/openvino-models",
        "/usr/local/lib64/audacity/openvino-models"
    };
}

juce::File StemModel::findModel()
{
    if (const auto named = juce::SystemStats::getEnvironmentVariable ("OPENDJ_STEM_MODEL", {});
        named.isNotEmpty())
        if (const juce::File file (named); file.existsAsFile())
            return file;

    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    const juce::File folders[]
    {
        home.getChildFile (".local/share/opendj/models"),
        home.getChildFile (".local/share/stemsep/models"),
        juce::File ("/usr/local/lib/openvino-models"),
        juce::File ("/usr/local/lib64/audacity/openvino-models")
    };

    for (const auto& folder : folders)
        for (const auto* name : { "htdemucs_v4.xml", "htdemucs.xml" })
            if (const auto file = folder.getChildFile (name); file.existsAsFile())
                return file;

    return {};
}

} // namespace opendj
