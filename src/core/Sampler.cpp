/*
    This file is part of OpenDJ. See LICENSE for terms (GPLv3 or later).
    Copyright (C) 2026 The OpenDJ contributors.
*/

#include "core/Sampler.h"

#include "analysis/TrackDecoder.h"

#include <algorithm>
#include <cmath>

namespace opendj
{

namespace
{
    // The same four point Hermite the decks use. A slot loaded at 44.1 kHz and
    // played out of a 48 kHz device is being resampled whether anyone thinks of
    // it that way or not, and a one shot with aliasing on it is audible over a
    // mix in a way it never is on its own.
    inline float interpolate (const float* data, juce::int64 numSamples, double position) noexcept
    {
        const auto i1 = static_cast<juce::int64> (std::floor (position));
        const auto frac = static_cast<float> (position - static_cast<double> (i1));

        const auto at = [data, numSamples] (juce::int64 i) noexcept -> float
        {
            return (i < 0 || i >= numSamples) ? 0.0f : data[i];
        };

        const auto xm1 = at (i1 - 1);
        const auto x0  = at (i1);
        const auto x1  = at (i1 + 1);
        const auto x2  = at (i1 + 2);

        const auto c = (x1 - xm1) * 0.5f;
        const auto v = x0 - x1;
        const auto w = c + v;
        const auto a = w + v + (x2 - x0) * 0.5f;
        const auto bNeg = w + a;

        return (((a * frac) - bNeg) * frac + c) * frac + x0;
    }

    // Long enough to swallow the step a sound starting or stopping mid waveform
    // would otherwise make, short enough that a stab still sounds like a stab.
    constexpr double fadeSeconds = 0.004;

    // A sampler holds stabs and loops, not tracks. Refusing a long file here is
    // kinder than letting somebody fill eight slots with albums.
    constexpr double maxSlotSeconds = 60.0;
}

//==============================================================================

Sampler::Sampler() { prepare (44100.0); }
Sampler::~Sampler() = default;

void Sampler::prepare (double sampleRate)
{
    deviceSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    envelopeStep = static_cast<float> (1.0 / (fadeSeconds * deviceSampleRate));
}

//==============================================================================

bool Sampler::loadSlot (int slot, juce::AudioFormatManager& formatManager,
                        const juce::File& file, juce::String* failureReason)
{
    auto decoded = TrackDecoder::decode (formatManager, file, failureReason);

    if (decoded == nullptr)
        return false;

    return installSlot (slot, std::move (decoded), file.getFileNameWithoutExtension(), failureReason);
}

bool Sampler::installSlot (int slot, std::unique_ptr<DecodedAudio> decoded,
                           const juce::String& name, juce::String* failureReason)
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
    {
        if (failureReason != nullptr)
            *failureReason = "There is no slot " + juce::String (slot + 1) + ".";

        return false;
    }

    if (decoded == nullptr)
        return false;

    if (decoded->lengthSeconds() > maxSlotSeconds)
    {
        if (failureReason != nullptr)
            *failureReason = name + " is longer than a minute, which is a track rather than a "
                             "sample. Load it onto a deck instead.";

        return false;
    }

    auto sound = std::make_shared<Sound>();
    sound->audio = std::move (decoded->audio);
    sound->sampleRate = decoded->sampleRate;
    sound->name = name;

    auto& s = slots[(size_t) slot];

    stop (slot);
    retire (slot);

    s.owned = std::move (sound);
    s.active.store (s.owned.get(), std::memory_order_release);

    return true;
}

void Sampler::clearSlot (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return;

    stop (slot);
    retire (slot);

    slots[(size_t) slot].positionSeconds.store (0.0, std::memory_order_relaxed);
}

void Sampler::retire (int slot)
{
    auto& s = slots[(size_t) slot];

    s.active.store (nullptr, std::memory_order_release);

    if (s.owned != nullptr)
        retired.emplace_back (std::move (s.owned), blocksProcessed.load (std::memory_order_relaxed));

    s.owned = nullptr;
}

void Sampler::cleanUp()
{
    const auto now = blocksProcessed.load (std::memory_order_relaxed);

    // Two blocks, for the reason the decks give: the audio thread reloads every
    // slot pointer at the top of processBlock, so by then it holds none of these.
    const auto safeToFree = [now] (const auto& entry) { return now - entry.second >= 2; };

    retired.erase (std::remove_if (retired.begin(), retired.end(), safeToFree), retired.end());
}

//==============================================================================

bool Sampler::isSlotLoaded (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots)
        && slots[(size_t) slot].active.load (std::memory_order_acquire) != nullptr;
}

juce::String Sampler::getSlotName (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return {};

    // The message thread owns this pointer, so reading through it is safe here
    // in a way that following the atomic would not be.
    const auto& owned = slots[(size_t) slot].owned;
    return owned != nullptr ? owned->name : juce::String();
}

double Sampler::getSlotLengthSeconds (int slot) const noexcept
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return 0.0;

    const auto& owned = slots[(size_t) slot].owned;

    return (owned != nullptr && owned->sampleRate > 0.0)
         ? owned->audio.getNumSamples() / owned->sampleRate
         : 0.0;
}

bool Sampler::isSlotPlaying (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots)
        && slots[(size_t) slot].playing.load (std::memory_order_relaxed);
}

double Sampler::getSlotPositionSeconds (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots)
         ? slots[(size_t) slot].positionSeconds.load (std::memory_order_relaxed)
         : 0.0;
}

void Sampler::trigger (int slot)
{
    if (! isSlotLoaded (slot))
        return;

    auto& s = slots[(size_t) slot];

    s.stopRequested.store (false, std::memory_order_relaxed);
    s.triggerCount.fetch_add (1, std::memory_order_release);
    s.playing.store (true, std::memory_order_relaxed);
}

void Sampler::stop (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return;

    slots[(size_t) slot].stopRequested.store (true, std::memory_order_relaxed);
}

void Sampler::stopAll()
{
    for (int slot = 0; slot < numSlots; ++slot)
        stop (slot);
}

void Sampler::setSlotGain (int slot, float gain)
{
    if (juce::isPositiveAndBelow (slot, numSlots))
        slots[(size_t) slot].gain.store (juce::jlimit (0.0f, 2.0f, gain), std::memory_order_relaxed);
}

float Sampler::getSlotGain (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots)
         ? slots[(size_t) slot].gain.load (std::memory_order_relaxed)
         : 0.0f;
}

void Sampler::setSlotLooping (int slot, bool shouldLoop)
{
    if (juce::isPositiveAndBelow (slot, numSlots))
        slots[(size_t) slot].looping.store (shouldLoop, std::memory_order_relaxed);
}

bool Sampler::isSlotLooping (int slot) const noexcept
{
    return juce::isPositiveAndBelow (slot, numSlots)
        && slots[(size_t) slot].looping.load (std::memory_order_relaxed);
}

void Sampler::setGain (float gain)
{
    masterGain.store (juce::jlimit (0.0f, 2.0f, gain), std::memory_order_relaxed);
}

//==============================================================================

void Sampler::processBlock (juce::AudioBuffer<float>& master,
                            juce::AudioBuffer<float>& cue,
                            int numSamples)
{
    blocksProcessed.fetch_add (1, std::memory_order_relaxed);

    if (numSamples <= 0 || master.getNumChannels() < 2)
        return;

    const auto toCue = cueEnabled.load (std::memory_order_relaxed) && cue.getNumChannels() >= 2;
    const auto gMaster = masterGain.load (std::memory_order_relaxed);

    for (auto& s : slots)
    {
        const auto* sound = s.active.load (std::memory_order_acquire);

        if (sound == nullptr || sound->audio.getNumSamples() <= 0)
        {
            s.playing.store (false, std::memory_order_relaxed);
            continue;
        }

        const auto triggers = s.triggerCount.load (std::memory_order_acquire);

        if (triggers != s.seenTriggers)
        {
            s.seenTriggers = triggers;
            s.position = 0.0;
            s.releasing = false;
            // The envelope is deliberately not reset. A retrigger over a sound
            // still ringing carries it across, so the new fade in does not chop
            // the tail of the tap before it.
        }
        else if (! s.playing.load (std::memory_order_relaxed))
        {
            continue;
        }

        if (s.stopRequested.load (std::memory_order_relaxed))
            s.releasing = true;

        const auto numFrames = (juce::int64) sound->audio.getNumSamples();
        const auto rate = sound->sampleRate / deviceSampleRate;
        const auto looping = s.looping.load (std::memory_order_relaxed);
        const auto gSlot = s.gain.load (std::memory_order_relaxed) * gMaster;

        const auto* left  = sound->audio.getReadPointer (0);
        const auto* right = sound->audio.getNumChannels() > 1 ? sound->audio.getReadPointer (1)
                                                              : left;

        auto* masterL = master.getWritePointer (0);
        auto* masterR = master.getWritePointer (1);
        auto* cueL = toCue ? cue.getWritePointer (0) : nullptr;
        auto* cueR = toCue ? cue.getWritePointer (1) : nullptr;

        auto finished = false;

        for (int i = 0; i < numSamples; ++i)
        {
            if (s.position >= (double) numFrames)
            {
                if (! looping) { finished = true; break; }

                s.position -= (double) numFrames;
            }

            // The fade out also runs the last few milliseconds of a one shot,
            // so a sound ending on a sample that is not zero does not click.
            const auto nearingEnd = ! looping
                                 && (double) numFrames - s.position < fadeSeconds * sound->sampleRate;

            const auto target = (s.releasing || nearingEnd) ? 0.0f : 1.0f;

            s.envelope = target > s.envelope ? juce::jmin (target, s.envelope + envelopeStep)
                                             : juce::jmax (target, s.envelope - envelopeStep);

            if (s.envelope <= 0.0f && target == 0.0f) { finished = true; break; }

            const auto g = s.envelope * gSlot;

            const auto l = interpolate (left, numFrames, s.position) * g;
            const auto r = interpolate (right, numFrames, s.position) * g;

            masterL[i] += l;
            masterR[i] += r;

            if (toCue) { cueL[i] += l; cueR[i] += r; }

            s.position += rate;
        }

        if (finished)
        {
            s.position = 0.0;
            s.envelope = 0.0f;
            s.releasing = false;
            s.stopRequested.store (false, std::memory_order_relaxed);
            s.playing.store (false, std::memory_order_relaxed);
            s.positionSeconds.store (0.0, std::memory_order_relaxed);
        }
        else
        {
            s.positionSeconds.store (s.position / sound->sampleRate, std::memory_order_relaxed);
        }
    }
}

} // namespace opendj
