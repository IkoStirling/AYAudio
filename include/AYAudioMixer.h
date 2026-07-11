#pragma once
// AYAudioMixer.h - Bus mixer + per-voice gain application
//
// Mixer applies user gains (voice volume * bus gain * master gain) and
// accumulates voices into the interleaved float32 output buffer. The mixer
// is intentionally backend-agnostic: it writes into a caller-provided buffer
// so the same code path runs against MiniaudioBackend (real device) and
// MockBackend (test capture).
//
// v0 covers the audio-graph "flat" mixing (no reverb send, no ducking).
// Ducking, reverb, and per-bus effects are scheduled for v2 (see design §4.3).

#include <AYCore.h>
#include <AYAudioTypes.h>
#include <array>
#include <cstdint>
#include <vector>

namespace ayt::audio
{

struct BusState {
    float gain = 1.0f;
    bool  mute = false;
    bool  solo = false;       // editor debug (v2)
};

class AudioMixer {
public:
    AudioMixer() = default;

    // Configure the mixer. outputChannels must be 1 or 2 for v0.
    bool initialize(uint32_t maxVoices, uint16_t outputChannels);

    // Reset mixer state (used during shutdown and engine reset).
    void reset();

    // ===== Bus control =====
    void setBusGain(AudioBus bus, float linearGain);
    void setMasterGain(float linearGain);
    float getBusGain(AudioBus bus) const;
    float getMasterGain() const { return _masterGain; }

    // ===== Mix step =====
    //
    // Mix `voiceCount` active voices from `voices` + `clips` into
    // `outputFrames` interleaved float32 frames. Channels must equal
    // _outputChannels.
    //
    // Each voice callback receives its assigned slice of output memory and
    // is expected to write float samples in [-1, +1]. The mixer applies bus
    // gain, master gain, and per-voice volume afterwards, then clamps.
    //
    // `voiceSource` is a function-like: given a voice index, fills
    // `voiceBuffer[voiceIndex]` with raw PCM mixed at the voice's spatial
    // gains. We use a template-style callback pointer to keep this header
    // header-only-friendly for the v0 mix step.

    using VoiceSourceFn = void(*)(void* userData,
                                  uint32_t voiceIndex,
                                  float* voiceBuffer,
                                  uint32_t frameCount);

    void mix(float* output,
             uint32_t outputFrames,
             const VoiceRuntime* voices,
             const VoiceSourceFn* voiceSources,
             void** voiceUserData,
             uint32_t voiceCount);

    // Returns a reference to the per-bus state for inspection.
    const BusState& getBusState(AudioBus bus) const;

    // Number of output channels configured.
    uint16_t outputChannels() const { return _outputChannels; }

private:
    std::array<BusState, static_cast<size_t>(AudioBus::Count)> _buses{};
    float _masterGain = 1.0f;
    uint16_t _outputChannels = 2;

    // Voice count is informational here — actual storage lives on the engine.
    uint32_t _maxVoices = 0;
};

} // namespace ayt::audio