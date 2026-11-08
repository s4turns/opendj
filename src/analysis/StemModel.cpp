/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "analysis/StemModel.h"

#include "analysis/StemDsp.h"

#include <array>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>

#if OPENDJ_HAVE_OPENVINO
 #include <openvino/openvino.hpp>
#endif

#if OPENDJ_HAVE_ONNXRUNTIME
 #include <onnxruntime_cxx_api.h>

 #if JUCE_WINDOWS
  #define NOMINMAX
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <dxgi.h>
  #pragma comment (lib, "dxgi.lib")
 #endif
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

    juce::Array<juce::File> modelFolders()
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        return
        {
            home.getChildFile (".local/share/opendj/models"),
            home.getChildFile (".local/share/stemsep/models"),
            juce::File ("/usr/local/lib/openvino-models"),
            juce::File ("/usr/local/lib64/audacity/openvino-models")
        };
    }

    juce::File findIn (const char* environmentVariable, std::initializer_list<const char*> names)
    {
        if (const auto named = juce::SystemStats::getEnvironmentVariable (environmentVariable, {});
            named.isNotEmpty())
            if (const juce::File file (named); file.existsAsFile())
                return file;

        for (const auto& folder : modelFolders())
            for (const auto* name : names)
                if (const auto file = folder.getChildFile (name); file.existsAsFile())
                    return file;

        return {};
    }
}

/** One engine: runs a single segment through the model. The inputs are the
    normalised spectrum [1, 4, freqBins, frames] and waveform [1, 2, segment];
    the outputs, valid until the next call, are the spectrum of all four stems
    [1, 16, freqBins, frames] and their waveforms [1, 8, segment]. */
struct StemModel::Backend
{
    virtual ~Backend() = default;

    virtual bool infer (float* spectrum, float* waveform,
                        const float*& spectrumOut, const float*& waveformOut) = 0;

    juce::String device;
    juce::File file;
};

namespace
{
#if OPENDJ_HAVE_OPENVINO
    struct OpenVinoBackend final : StemModel::Backend
    {
        ov::Core core;
        ov::CompiledModel compiled;
        ov::InferRequest request;

        // Which output is which, decided by shape rather than by name: the
        // names in the converted graph are numbers that change with the
        // conversion.
        size_t spectrumOutput = 0;
        size_t waveformOutput = 1;

        /** Throws when the model cannot be read or compiled. */
        explicit OpenVinoBackend (const juce::File& modelXml)
        {
            file = modelXml;
            auto model = core.read_model (modelXml.getFullPathName().toStdString());

            // OpenVINO's GPU is Intel graphics only, several times faster
            // where there is one; a machine without one separates on the CPU.
            const auto devices = core.get_available_devices();
            const auto hasGpu = std::find (devices.begin(), devices.end(), "GPU") != devices.end();

            device = hasGpu ? "GPU" : "CPU";

            try
            {
                compiled = core.compile_model (model, device.toStdString());
            }
            catch (const std::exception&)
            {
                device = "CPU";
                compiled = core.compile_model (model, "CPU");
            }

            device << " (OpenVINO)";
            request = compiled.create_infer_request();

            const auto outputs = compiled.outputs();

            if (outputs.size() != 2)
                throw std::runtime_error ("the model has " + std::to_string (outputs.size())
                                          + " outputs, expected 2");

            // The spectrum output is four dimensional, the waveform three.
            spectrumOutput = outputs[0].get_shape().size() == 4 ? 0 : 1;
            waveformOutput = 1 - spectrumOutput;
        }

        bool infer (float* spectrum, float* waveform,
                    const float*& spectrumOut, const float*& waveformOut) override
        {
            try
            {
                request.set_input_tensor (0, ov::Tensor (ov::element::f32,
                                                         { 1, 4, (size_t) dsp::freqBins, (size_t) dsp::frames },
                                                         spectrum));
                request.set_input_tensor (1, ov::Tensor (ov::element::f32,
                                                         { 1, 2, (size_t) dsp::segment },
                                                         waveform));
                request.infer();
            }
            catch (const std::exception&)
            {
                return false;
            }

            spectrumOut = request.get_output_tensor (spectrumOutput).data<const float>();
            waveformOut = request.get_output_tensor (waveformOutput).data<const float>();
            return true;
        }
    };
#endif

#if OPENDJ_HAVE_ONNXRUNTIME
   #if JUCE_WINDOWS
    /** setup-windows.ps1 unpacks NVIDIA's runtime libraries here rather than
        installing the CUDA toolkit. The CUDA provider and cuDNN load them, and
        cuDNN its own parts, with ordinary LoadLibrary calls, so the folder goes
        on this process's PATH before the provider is asked for. */
    void addCudaLibrariesToPath()
    {
        const auto bin = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                             .getChildFile (".local/share/opendj/cuda/bin");

        if (! bin.isDirectory())
            return;

        const auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});

        if (! path.containsIgnoreCase (bin.getFullPathName()))
            SetEnvironmentVariableW (L"PATH", (bin.getFullPathName() + ";" + path).toWideCharPointer());
    }

    /** The card CUDA's device 0 most likely is: the first NVIDIA adapter. */
    juce::String nvidiaAdapterName()
    {
        IDXGIFactory1* factory = nullptr;

        if (FAILED (CreateDXGIFactory1 (__uuidof (IDXGIFactory1), (void**) &factory)))
            return {};

        juce::String name;
        IDXGIAdapter1* adapter = nullptr;

        for (UINT i = 0; name.isEmpty() && factory->EnumAdapters1 (i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 description {};

            if (SUCCEEDED (adapter->GetDesc1 (&description)) && description.VendorId == 0x10de)
                name = juce::String (description.Description);

            adapter->Release();
        }

        factory->Release();
        return name;
    }
   #endif

   #if JUCE_MAC
    /** Where Core ML keeps the model it compiled. Compiling takes 20 to 30
        seconds on every load otherwise, against a few with the cache.

        ONNX Runtime keys its cache on the model's path alone, so a newer model
        dropped over the old one would quietly run the old one's compilation.
        The folder is named after the file's size and date for that reason, and
        an older model's folder, some 600 MB, is removed when a new one is made. */
    juce::File coreMLCacheFolder (const juce::File& modelOnnx)
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_CACHE_HOME", {});
        const auto root = (xdg.isNotEmpty() ? juce::File (xdg) : home.getChildFile (".cache"))
                              .getChildFile ("opendj").getChildFile ("coreml");

        const auto key = modelOnnx.getFileNameWithoutExtension() + "-"
                       + juce::String (modelOnnx.getSize()) + "-"
                       + juce::String (modelOnnx.getLastModificationTime().toMilliseconds());
        const auto folder = root.getChildFile (key);

        if (! folder.isDirectory())
        {
            for (const auto& old : root.findChildFiles (juce::File::findDirectories, false,
                                                        modelOnnx.getFileNameWithoutExtension() + "-*"))
                old.deleteRecursively();

            folder.createDirectory();
        }

        return folder;
    }
   #endif

    struct OnnxRuntimeBackend final : StemModel::Backend
    {
        Ort::Env environment { ORT_LOGGING_LEVEL_ERROR, "OpenDJ" };
        Ort::Session session { nullptr };
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> outputs;

        size_t spectrumOutput = 0;
        size_t waveformOutput = 1;

        /** Throws when the model cannot be loaded on a GPU. There is no point
            running this one on the CPU: OpenVINO does that, faster. */
        explicit OnnxRuntimeBackend (const juce::File& modelOnnx)
        {
            file = modelOnnx;
            Ort::SessionOptions options;

           #if JUCE_MAC
            // Core ML picks the GPU or the Neural Engine itself, and hands back
            // to the CPU what it cannot run: 25 of htdemucs's 1423 nodes. On an
            // M5 Pro a six minute track separates in about 26 seconds this way,
            // against 2.5 times that on the CPU alone.
            std::unordered_map<std::string, std::string> coreML { { "ModelFormat", "MLProgram" },
                                                                  { "MLComputeUnits", "ALL" } };

            if (const auto cache = coreMLCacheFolder (modelOnnx); cache.isDirectory())
                coreML["ModelCacheDirectory"] = cache.getFullPathName().toStdString();

            options.AppendExecutionProvider ("CoreML", coreML);
            device = "Apple GPU (Core ML)";
           #else
            #if JUCE_WINDOWS
             addCudaLibrariesToPath();
            #endif

            // Throws when the provider, or the CUDA and cuDNN libraries it is
            // linked against, cannot be loaded: no NVIDIA card, or no CUDA.
            OrtCUDAProviderOptions cuda;
            options.AppendExecutionProvider_CUDA (cuda);

            #if JUCE_WINDOWS
             device = nvidiaAdapterName();
            #endif

            if (device.isEmpty())
                device = "NVIDIA GPU";

            device << " (CUDA)";
           #endif

           #if JUCE_WINDOWS
            session = Ort::Session (environment, modelOnnx.getFullPathName().toWideCharPointer(), options);
           #else
            session = Ort::Session (environment, modelOnnx.getFullPathName().toRawUTF8(), options);
           #endif

            if (session.GetOutputCount() != 2)
                throw std::runtime_error ("the model has " + std::to_string (session.GetOutputCount())
                                          + " outputs, expected 2");

            spectrumOutput = session.GetOutputTypeInfo (0).GetTensorTypeAndShapeInfo().GetDimensionsCount() == 4 ? 0 : 1;
            waveformOutput = 1 - spectrumOutput;
        }

        bool infer (float* spectrum, float* waveform,
                    const float*& spectrumOut, const float*& waveformOut) override
        {
            const std::array<int64_t, 4> spectrumShape { 1, 4, dsp::freqBins, dsp::frames };
            const std::array<int64_t, 3> waveformShape { 1, 2, dsp::segment };

            std::array<Ort::Value, 2> inputs
            {
                Ort::Value::CreateTensor<float> (memory, spectrum, (size_t) (4 * dsp::freqBins * dsp::frames),
                                                 spectrumShape.data(), spectrumShape.size()),
                Ort::Value::CreateTensor<float> (memory, waveform, (size_t) (2 * dsp::segment),
                                                 waveformShape.data(), waveformShape.size())
            };

            const char* const inputNames[] { "x", "xt" };
            const char* const outputNames[] { "x_out", "xt_out" };

            try
            {
                outputs = session.Run (Ort::RunOptions { nullptr }, inputNames, inputs.data(), inputs.size(),
                                       outputNames, 2);
            }
            catch (const std::exception&)
            {
                return false;
            }

            spectrumOut = outputs[spectrumOutput].GetTensorData<float>();
            waveformOut = outputs[waveformOutput].GetTensorData<float>();
            return true;
        }
    };
#endif
}

StemModel::StemModel() = default;
StemModel::~StemModel() = default;

bool StemModel::isLoaded() const                { return backend != nullptr; }
juce::String StemModel::getDeviceName() const   { return backend != nullptr ? backend->device : juce::String(); }
juce::File StemModel::getModelFile() const      { return backend != nullptr ? backend->file : juce::File(); }
juce::String StemModel::getFallbackReason() const { return fallbackReason; }

juce::String StemModel::load (Engine engine)
{
    backend.reset();
    fallbackReason = {};

    juce::String error;

    if (engine != Engine::openVino)
    {
       #if OPENDJ_HAVE_ONNXRUNTIME
        if (const auto file = findOnnxModel(); file.existsAsFile())
        {
            try
            {
                backend = std::make_unique<OnnxRuntimeBackend> (file);
                return {};
            }
            catch (const std::exception& e)
            {
                error = juce::String ("Could not use the GPU through ONNX Runtime: ") + e.what();
            }
        }
        else
        {
            error = "No htdemucs_v4.onnx found. Looked in: " + getSearchedLocations().joinIntoString (", ");
        }
       #else
        error = "OpenDJ was built without ONNX Runtime.";
       #endif

        if (engine == Engine::onnxRuntime)
            return error;
    }

   #if OPENDJ_HAVE_OPENVINO
    const auto file = findModel();

    if (! file.existsAsFile())
        return "No htdemucs model found. Looked in: " + getSearchedLocations().joinIntoString (", ");

    try
    {
        backend = std::make_unique<OpenVinoBackend> (file);

        // Only worth saying when there was a GPU engine to fall back from.
       #if OPENDJ_HAVE_ONNXRUNTIME
        if (engine == Engine::automatic && findOnnxModel().existsAsFile())
            fallbackReason = error;
       #endif

        return {};
    }
    catch (const std::exception& e)
    {
        return juce::String ("Could not load the model: ") + e.what();
    }
   #else
    if (engine == Engine::openVino)
        return "OpenDJ was built without OpenVINO.";

   #if OPENDJ_HAVE_ONNXRUNTIME
    return error;
   #else
    return "OpenDJ was built without OpenVINO or ONNX Runtime, so it cannot separate stems itself.";
   #endif
   #endif
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

        const float* specData = nullptr;
        const float* timeData = nullptr;

        if (! backend->infer (specInput.data(), timeInput.data(), specData, timeData))
            return false;

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

juce::StringArray StemModel::getSearchedLocations()
{
    juce::StringArray locations { "$OPENDJ_STEM_MODEL", "$OPENDJ_STEM_MODEL_ONNX" };

    for (const auto& folder : modelFolders())
        locations.add (folder.getFullPathName());

    return locations;
}

juce::File StemModel::findModel()
{
    return findIn ("OPENDJ_STEM_MODEL", { "htdemucs_v4.xml", "htdemucs.xml" });
}

juce::File StemModel::findOnnxModel()
{
    return findIn ("OPENDJ_STEM_MODEL_ONNX", { "htdemucs_v4.onnx" });
}

} // namespace opendj
