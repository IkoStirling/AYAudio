#include <AYAudio/AudioMixer.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace ayt::audio
{

namespace {

// Schroeder reverberator — 2 presets. All delay values in seconds.
// combFeedback controls T60; allpassGain shapes the early reflections.
// We mirror AudioMixer's private line counts with file-local constants so
// the preset table can be built in this anonymous namespace without
// exposing the class's private statics.
constexpr uint32_t kCombCount    = 4;
constexpr uint32_t kAllpassCount = 2;

struct ReverbPreset {
    float combDelaysSec[kCombCount];
    float combFeedback[kCombCount];
    float allpassDelaysSec[kAllpassCount];
    float allpassGain;
};

constexpr ReverbPreset kReverbPresets[] = {
    // Preset 0 — small room (tight comb delays, low feedback).
    {
        { 0.0297f, 0.0371f, 0.0411f, 0.0437f },
        { 0.84f,   0.83f,   0.82f,   0.81f },
        { 0.0050f, 0.0017f },
        0.7f,
    },
    // Preset 1 — hall (longer comb delays, higher feedback).
    {
        { 0.0453f, 0.0521f, 0.0607f, 0.0673f },
        { 0.90f,   0.90f,   0.89f,   0.88f },
        { 0.0050f, 0.0017f },
        0.7f,
    },
};

constexpr uint32_t kReverbPresetCount =
    sizeof(kReverbPresets) / sizeof(kReverbPresets[0]);

} // namespace

bool AudioMixer::initialize(uint32_t maxVoices, uint16_t outputChannels, uint32_t sampleRate) {
    if (outputChannels != 1 && outputChannels != 2) {
        return false;
    }
    if (sampleRate == 0) {
        return false;
    }
    _maxVoices = maxVoices;
    _outputChannels = outputChannels;
    _sampleRate = sampleRate;
    _masterGain = 1.0f;

    for (auto& bus : _buses) {
        bus.gain = 1.0f;
        bus.mute = false;
        bus.solo = false;
    }

    _duck = DuckingState{};
    _busPeaks.fill(0);

    // v2.0b: size reverb delay lines from the active preset. Clamp the
    // preset index so a caller-supplied garbage value falls back to 0.
    if (_reverbPreset >= kReverbPresetCount) {
        _reverbPreset = 0;
    }
    const ReverbPreset& preset = kReverbPresets[_reverbPreset];

    auto sizeDelayLine = [sampleRate](DelayLine& line, float delaySec, float feedback) {
        uint32_t samples = std::max<uint32_t>(2, static_cast<uint32_t>(std::round(delaySec * static_cast<float>(sampleRate))));
        if (samples < 2) samples = 2;
        line.buf.assign(samples, 0.0f);
        line.idx = 0;
        line.delaySamples = samples;
        line.feedback = feedback;
    };

    auto sizeAllpass = [sampleRate](DelayLine& line, float delaySec, float gain) {
        uint32_t samples = std::max<uint32_t>(2, static_cast<uint32_t>(std::round(delaySec * static_cast<float>(sampleRate))));
        if (samples < 2) samples = 2;
        line.buf.assign(samples, 0.0f);
        line.idx = 0;
        line.delaySamples = samples;
        // feedback on allpass lines stores the allpass gain coefficient.
        line.feedback = gain;
    };

    for (uint32_t i = 0; i < kCombCount; ++i) {
        sizeDelayLine(_combL[i], preset.combDelaysSec[i], preset.combFeedback[i]);
        sizeDelayLine(_combR[i], preset.combDelaysSec[i], preset.combFeedback[i]);
    }
    for (uint32_t i = 0; i < kAllpassCount; ++i) {
        sizeAllpass(_allpassL[i], preset.allpassDelaysSec[i], preset.allpassGain);
        sizeAllpass(_allpassR[i], preset.allpassDelaysSec[i], preset.allpassGain);
    }

    // Wet-send accumulator sized to the audio-thread frame cap so the hot
    // path never re-allocates.
    _reverbSendBuffer.assign(2 * 4096, 0.0f);

    // Master is index 0; convention: Master bus starts at 1.0 (already set).
    return true;
}

void AudioMixer::reset() {
    _busPeaks.fill(0);
    _outputLevels = {};
    _masterGain = 1.0f;
    for (auto& bus : _buses) {
        bus.gain = 1.0f;
        bus.mute = false;
        bus.solo = false;
    }
    _duck = DuckingState{};
    _resetReverbTails();
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

void AudioMixer::setDucking(float depthDb, float attackPerSec, float releasePerSec) {
    _duck.depthDb       = std::min(0.0f, depthDb);
    _duck.attackPerSec  = std::max(0.001f, attackPerSec);
    _duck.releasePerSec = std::max(0.001f, releasePerSec);
}

void AudioMixer::integrateDuckEnvelope(float dt) {
    if (dt <= 0.0f) {
        return;
    }
    const bool voiceActive =
        (_duck.activeBusMask & (1u << static_cast<uint32_t>(AudioBus::Voice))) != 0;
    const float targetDb = voiceActive ? _duck.depthDb : 0.0f;
    // Going deeper (currentDb > target) → use attack rate; coming back
    // up (currentDb < target) → use release rate.
    const float rate = (_duck.currentDb > targetDb)
                           ? _duck.attackPerSec
                           : _duck.releasePerSec;
    const float delta = rate * dt;
    if (_duck.currentDb > targetDb) {
        _duck.currentDb = std::max(targetDb, _duck.currentDb - delta);
    } else {
        _duck.currentDb = std::min(targetDb, _duck.currentDb + delta);
    }
}

void AudioMixer::setMasterReverbReturn(float linearReturn) {
    _reverbReturn = std::clamp(linearReturn, 0.0f, 1.0f);
}

void AudioMixer::setReverbPreset(uint8_t preset) {
    if (preset >= kReverbPresetCount) {
        preset = 0;
    }
    if (_reverbPreset == preset) {
        return;
    }
    _reverbPreset = preset;
    // Resize delay lines for the new preset; keep their contents (we only
    // swap parameters, the tail will re-shape over a few frames).
    const ReverbPreset& p = kReverbPresets[_reverbPreset];
    auto resizeComb = [this](DelayLine& line, float delaySec, float feedback) {
        const uint32_t samples = std::max<uint32_t>(
            2, static_cast<uint32_t>(std::round(delaySec * static_cast<float>(_sampleRate))));
        line.buf.assign(samples, 0.0f);
        line.idx = 0;
        line.delaySamples = samples;
        line.feedback = feedback;
    };
    auto resizeAllpass = [this](DelayLine& line, float delaySec, float gain) {
        const uint32_t samples = std::max<uint32_t>(
            2, static_cast<uint32_t>(std::round(delaySec * static_cast<float>(_sampleRate))));
        line.buf.assign(samples, 0.0f);
        line.idx = 0;
        line.delaySamples = samples;
        line.feedback = gain;
    };
    for (uint32_t i = 0; i < kCombCount; ++i) {
        resizeComb(_combL[i], p.combDelaysSec[i], p.combFeedback[i]);
        resizeComb(_combR[i], p.combDelaysSec[i], p.combFeedback[i]);
    }
    for (uint32_t i = 0; i < kAllpassCount; ++i) {
        resizeAllpass(_allpassL[i], p.allpassDelaysSec[i], p.allpassGain);
        resizeAllpass(_allpassR[i], p.allpassDelaysSec[i], p.allpassGain);
    }
}

void AudioMixer::_resetReverbTails() {
    for (auto& line : _combL)     std::fill(line.buf.begin(),     line.buf.end(),     0.0f);
    for (auto& line : _combR)     std::fill(line.buf.begin(),     line.buf.end(),     0.0f);
    for (auto& line : _allpassL)  std::fill(line.buf.begin(),     line.buf.end(),     0.0f);
    for (auto& line : _allpassR)  std::fill(line.buf.begin(),     line.buf.end(),     0.0f);
    for (auto& line : _combL)     line.idx = 0;
    for (auto& line : _combR)     line.idx = 0;
    for (auto& line : _allpassL)  line.idx = 0;
    for (auto& line : _allpassR)  line.idx = 0;
    if (!_reverbSendBuffer.empty()) {
        std::fill(_reverbSendBuffer.begin(), _reverbSendBuffer.end(), 0.0f);
    }
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

    _outputLevels = {};
    // Zero the output buffer first; voices accumulate into it.
    const size_t totalSamples = static_cast<size_t>(outputFrames) * _outputChannels;
    std::memset(output, 0, totalSamples * sizeof(float));

    // Cap frames before any work so the reverb-send buffer stride matches.
    constexpr uint32_t kMaxFramesPerCall = 4096;
    if (outputFrames > kMaxFramesPerCall) {
        outputFrames = kMaxFramesPerCall;
    }

    // v2.0b ducking probe: clear at the top of the block; OR'd into for
    // every voice. Engine integrates the envelope after this call returns.
    _duck.activeBusMask = 0;
    _busPeaks.fill(0);
    for(auto& bus:_busMeterSamples) std::fill_n(bus.begin(),outputFrames*_outputChannels,0.f);

    // v2.0b reverb wet-send accumulator: clear at the top, populate per
    // voice (when reverbSend > 0), process through Schroeder network,
    // accumulate into output. Pre-allocated to the 4096-frame cap.
    // Even with zero voices we still process (zero send) so delay-line
    // energy can ring out as a reverb tail.
    if (!_reverbSendBuffer.empty()) {
        std::memset(_reverbSendBuffer.data(), 0,
                    2 * outputFrames * sizeof(float));
    }

    if (voiceCount > 0 && voices != nullptr && voiceSources != nullptr) {
        // v2.0b duck linear scale (Music bus only). Computed once per block
        // from the running envelope; the envelope integrates AFTER this call.
        const float duckLinear =
            (_duck.currentDb <= -60.0f)
                ? 0.0f
                : std::pow(10.0f, _duck.currentDb / 20.0f);

        float scratch[8192];

        for (uint32_t i = 0; i < voiceCount; ++i) {
            const VoiceSourceFn source = voiceSources[i];
            if (source == nullptr) {
                continue;
            }

            // Render the voice's PCM into the scratch buffer at full scale;
            // the voice source callback already applied its spatial gains.
            std::memset(scratch, 0, sizeof(scratch));
            source(voiceUserData[i], i, scratch, outputFrames);

            // Apply bus gain, master gain, ducking (Music bus only), and
            // per-voice volume.
            const size_t busIdx = static_cast<size_t>(voices[i].bus);
            const float busGain = (busIdx < _buses.size()) ? _buses[busIdx].gain : 1.0f;
            const float voiceGain = voices[i].volume;
            const float duckMul =
                (voices[i].bus == AudioBus::Music) ? duckLinear : 1.0f;
            const float totalGain = busGain * voiceGain * _masterGain * duckMul;

            if(busIdx<_busMeterSamples.size()) for(uint32_t sample=0;sample<outputFrames*_outputChannels;++sample)
                _busMeterSamples[busIdx][sample]+=scratch[sample]*totalGain;

            // Probe: mark which buses had an active voice this block.
            if (busIdx < 32) {
                _duck.activeBusMask |= (1u << busIdx);
            }

            if (_outputChannels == 1) {
                for (uint32_t f = 0; f < outputFrames; ++f) {
                    output[f] += scratch[f] * totalGain;
                }
            } else {
                for (uint32_t f = 0; f < outputFrames; ++f) {
                    output[2 * f + 0] += scratch[2 * f + 0] * totalGain;
                    output[2 * f + 1] += scratch[2 * f + 1] * totalGain;
                }
            }

            // v2.0b reverb send: if the voice wants a non-zero send, copy a
            // scaled version of its (spatial-gain-applied) dry signal into
            // the wet-send accumulator. We scale by bus gain + voice volume
            // (the same totalGain that just hit the dry output) so the wet
            // level tracks ducking and bus changes.
            const float reverbSend = voices[i].spatial.reverbSend;
            if (reverbSend > 0.0f && !_reverbSendBuffer.empty()) {
                const float wetScale = totalGain * reverbSend * _reverbReturn;
                float* wetBuf = _reverbSendBuffer.data();
                if (_outputChannels == 1) {
                    for (uint32_t f = 0; f < outputFrames; ++f) {
                        wetBuf[f] += scratch[f] * wetScale;
                    }
                } else {
                    for (uint32_t f = 0; f < outputFrames; ++f) {
                        wetBuf[2 * f + 0] += scratch[2 * f + 0] * wetScale;
                        wetBuf[2 * f + 1] += scratch[2 * f + 1] * wetScale;
                    }
                }
            }
        }
    }

    for(size_t bus=0;bus<_busPeaks.size();++bus) for(uint32_t sample=0;sample<outputFrames*_outputChannels;++sample)
        _busPeaks[bus]=std::max(_busPeaks[bus],std::abs(_busMeterSamples[bus][sample]));

    // Process the wet-send accumulator through the Schroeder network and
    // mix the result into output. Stereo only — mono paths skip reverb
    // (we'd need a mono comb matrix; not in v2.0b scope).
    if (_outputChannels == 2 && !_reverbSendBuffer.empty()) {
        _processReverbWet(_reverbSendBuffer.data(), output, outputFrames);
    }

    // Clamp once per output frame to prevent inter-voice summation from
    // exceeding [-1, +1] (which would otherwise clip in the DAC).
    for (size_t i = 0; i < totalSamples; ++i) {
        const float peak = std::abs(output[i]);
        if (_outputChannels == 1 || i % _outputChannels == 0)
            _outputLevels.left = std::max(_outputLevels.left, peak);
        else _outputLevels.right = std::max(_outputLevels.right, peak);
        _outputLevels.clipped |= peak > 1.0f;
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
    }
    if (_outputChannels == 1) _outputLevels.right = _outputLevels.left;
}

// Process the wet-send accumulator through 4 parallel combs + 2 series
// allpasses per channel (Schroeder). Each line uses an integer delay tap
// sized at initialize() time. Output is summed into `output` (the dry
// signal is already there). Deterministic — same input bytes produce the
// same output bytes frame-by-frame.
void AudioMixer::_processReverbWet(const float* sendBuffer, float* output, uint32_t frames) {
    if (frames == 0) {
        return;
    }

    auto stepComb = [](DelayLine& line, float in) -> float {
        const float y = line.buf[line.idx];
        line.buf[line.idx] = in + y * line.feedback;
        line.idx = (line.idx + 1) % line.delaySamples;
        return y;
    };
    auto stepAllpass = [](DelayLine& line, float in) -> float {
        const float buf = line.buf[line.idx];
        const float out = -line.feedback * in + buf;
        line.buf[line.idx] = in + line.feedback * out;
        line.idx = (line.idx + 1) % line.delaySamples;
        return out;
    };

    for (uint32_t f = 0; f < frames; ++f) {
        const float dryL = sendBuffer[2 * f + 0];
        const float dryR = sendBuffer[2 * f + 1];

        float wetL = (stepComb(_combL[0], dryL) +
                      stepComb(_combL[1], dryL) +
                      stepComb(_combL[2], dryL) +
                      stepComb(_combL[3], dryL)) * 0.25f;
        wetL = stepAllpass(_allpassL[0], wetL);
        wetL = stepAllpass(_allpassL[1], wetL);

        float wetR = (stepComb(_combR[0], dryR) +
                      stepComb(_combR[1], dryR) +
                      stepComb(_combR[2], dryR) +
                      stepComb(_combR[3], dryR)) * 0.25f;
        wetR = stepAllpass(_allpassR[0], wetR);
        wetR = stepAllpass(_allpassR[1], wetR);

        output[2 * f + 0] += wetL;
        output[2 * f + 1] += wetR;
    }
}

} // namespace ayt::audio
