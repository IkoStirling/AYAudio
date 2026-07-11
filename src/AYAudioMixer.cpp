#include <AYAudioMixer.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace ayt::audio
{

bool AudioMixer::initialize(uint32_t maxVoices, uint16_t outputChannels) {
    if (outputChannels != 1 && outputChannels != 2) {
        return false; // v0 supports mono + stereo only
    }
    _maxVoices = maxVoices;
    _outputChannels = outputChannels;
    _masterGain = 1.0f;

    for (auto& bus : _buses) {
        bus.gain = 1.0f;
        bus.mute = false;
        bus.solo = false;
    }
    // Master is index 0; convention: Master bus starts at 1.0 (already set).
    return true;
}

void AudioMixer::reset() {
    _masterGain = 1.0f;
    for (auto& bus : _buses) {
        bus.gain = 1.0f;
        bus.mute = false;
        bus.solo = false;
    }
}

void AudioMixer::setBusGain(AudioBus bus, float linearGain) {
    const size_t idx = static_cast<size_t>(bus);
    if (idx >= _buses.size()) {
        return;
    }
    _buses[idx].gain = std::clamp(linearGain, 0.0f, 8.0f);
}

void AudioMixer::setMasterGain(float linearGain) {
    _masterGain = std::clamp(linearGain, 0.0f, 8.0f);
}

float AudioMixer::getBusGain(AudioBus bus) const {
    const size_t idx = static_cast<size_t>(bus);
    if (idx >= _buses.size()) {
        return 0.0f;
    }
    return _buses[idx].gain;
}

const BusState& AudioMixer::getBusState(AudioBus bus) const {
    const size_t idx = static_cast<size_t>(bus);
    if (idx >= _buses.size()) {
        return _buses[0]; // safe fallback
    }
    return _buses[idx];
}

void AudioMixer::mix(float* output,
                     uint32_t outputFrames,
                     const VoiceRuntime* voices,
                     const VoiceSourceFn* voiceSources,
                     void** voiceUserData,
                     uint32_t voiceCount) {
    if (output == nullptr || outputFrames == 0 || _outputChannels == 0) {
        return;
    }

    // Zero the output buffer first; voices accumulate into it.
    const size_t totalSamples = static_cast<size_t>(outputFrames) * _outputChannels;
    std::memset(output, 0, totalSamples * sizeof(float));

    if (voiceCount == 0 || voices == nullptr || voiceSources == nullptr) {
        return;
    }

    // We allocate per-voice buffers lazily to avoid hot-path allocations.
    // For v0 we mix voice-by-voice into the output buffer directly, but
    // each voice may need its own scratch buffer to apply per-voice gains
    // before bus/master scaling. We use a stack-friendly single-voice
    // scratch block and mix voice-by-voice.
    //
    // For real workloads the audio thread stack is fine for this small
    // scratch; for very large frame counts (>4096), a heap scratch would
    // be more robust. v0 caps frames at 4096 by convention.
    constexpr uint32_t kMaxFramesPerCall = 4096;
    if (outputFrames > kMaxFramesPerCall) {
        outputFrames = kMaxFramesPerCall;
    }

    // Per-voice scratch (mono). Voices with stereo source are downmixed
    // inside the voice source callback if needed.
    float scratch[4096];

    for (uint32_t i = 0; i < voiceCount; ++i) {
        const VoiceSourceFn source = voiceSources[i];
        if (source == nullptr) {
            continue;
        }

        // Render the voice's PCM into the scratch buffer at full scale;
        // the voice source callback already applied its spatial gains.
        std::memset(scratch, 0, sizeof(scratch));
        source(voiceUserData[i], i, scratch, outputFrames);

        // Apply bus gain, master gain, and per-voice volume.
        const size_t busIdx = static_cast<size_t>(voices[i].bus);
        const float busGain = (busIdx < _buses.size()) ? _buses[busIdx].gain : 1.0f;
        const float voiceGain = voices[i].volume;
        const float totalGain = busGain * voiceGain * _masterGain;

        if (_outputChannels == 1) {
            for (uint32_t f = 0; f < outputFrames; ++f) {
                output[f] += scratch[f] * totalGain;
            }
        } else {
            // For stereo the voice source callback is expected to produce
            // interleaved L/R frames in `scratch`. We sum them per-frame.
            for (uint32_t f = 0; f < outputFrames; ++f) {
                output[2 * f + 0] += scratch[2 * f + 0] * totalGain;
                output[2 * f + 1] += scratch[2 * f + 1] * totalGain;
            }
        }
    }

    // Clamp once per output frame to prevent inter-voice summation from
    // exceeding [-1, +1] (which would otherwise clip in the DAC).
    for (size_t i = 0; i < totalSamples; ++i) {
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
    }
}

} // namespace ayt::audio