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
// v2.0b adds:
//   - Ducking (Music bus sidechain, exponential ramp, dB depth)
//   - Reverb send (Schroeder bus-send, master wet return)
//   - Custom attenuation curves + doppler (in spatializer, not here)

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

struct DuckingState {
    float depthDb       = 0.0f;   // ≤ 0 dB; 0 = inactive (Music un-attenuated)
    float attackPerSec  = 8.0f;   // upward ramp rate (dB/s) when engaging
    float releasePerSec = 2.0f;   // downward ramp rate (dB/s) when releasing
    float currentDb     = 0.0f;   // running envelope (≤ 0 dB)
    // Per-mix-call probe: bit i set ⇒ bus i had at least one active voice.
    // Cleared at the top of mix(); OR'd into inside the per-voice loop.
    uint32_t activeBusMask = 0;
};

class AudioMixer {
public:
    AudioMixer() = default;

    // Configure the mixer. outputChannels must be 1 or 2.
    // sampleRate is used to size the reverb comb/allpass delay lines.
    bool initialize(uint32_t maxVoices, uint16_t outputChannels, uint32_t sampleRate);

    // Reset mixer state (used during shutdown and engine reset). Clears
    // ducking envelope + reverb tail buffers to zero.
    void reset();

    // ===== Bus control =====
    void setBusGain(AudioBus bus, float linearGain);
    void setMasterGain(float linearGain);
    float getBusGain(AudioBus bus) const;
    float getMasterGain() const { return _masterGain; }

    // ===== Ducking (v2.0b) =====
    void setDucking(float depthDb, float attackPerSec, float releasePerSec);
    const DuckingState& duckState() const { return _duck; }
    // Engine call: integrate currentDb toward target (target = depthDb if
    // Voice bus had any active voice this block, else 0). dt is in seconds.
    void integrateDuckEnvelope(float dt);

    // ===== Reverb send (v2.0b) =====
    void setMasterReverbReturn(float linearReturn);
    float reverbReturn() const { return _reverbReturn; }
    void setReverbPreset(uint8_t preset);
    uint8_t reverbPreset() const { return _reverbPreset; }

    // ===== Mix step =====
    //
    // Mix `voiceCount` active voices from `voices` into `outputFrames`
    // interleaved float32 frames. Channels must equal `_outputChannels`.
    //
    // Each voice callback receives its assigned slice of output memory and
    // is expected to write float samples in [-1, +1]. The mixer applies bus
    // gain, master gain, ducking (Music bus), per-voice volume, reverb send,
    // then clamps.

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
    uint32_t outputSampleRate() const { return _sampleRate; }

private:
    // Reset reverb tail buffers + duck envelope to zero.
    void _resetReverbTails();

    // Process the wet send accumulator through the Schroeder network and
    // accumulate the result into `output`. Stereo only (matches default
    // outputChannels == 2 path used by mix()).
    void _processReverbWet(const float* sendBuffer, float* output, uint32_t frames);

    std::array<BusState, static_cast<size_t>(AudioBus::Count)> _buses{};
    float _masterGain = 1.0f;
    uint16_t _outputChannels = 2;
    uint32_t _sampleRate = 48000;

    // Voice count is informational here — actual storage lives on the engine.
    uint32_t _maxVoices = 0;

    // v2.0b ducking envelope + per-block probe.
    DuckingState _duck{};

    // v2.0b reverb (Schroeder reverberator). Buffers are sized in
    // initialize() and never re-allocated.
    static constexpr uint32_t kReverbCombCount    = 4;
    static constexpr uint32_t kReverbAllpassCount = 2;
    float _reverbReturn = 0.25f;
    uint8_t _reverbPreset = 0;

    // Comb filter delay lines, one per channel × per comb. delaySamples[]
    // is the line's length; idx is the read/write cursor.
    struct DelayLine {
        std::vector<float> buf;
        uint32_t idx = 0;
        uint32_t delaySamples = 0;
        float feedback = 0.0f;
    };
    std::array<DelayLine, kReverbCombCount> _combL{};
    std::array<DelayLine, kReverbCombCount> _combR{};
    std::array<DelayLine, kReverbAllpassCount> _allpassL{};
    std::array<DelayLine, kReverbAllpassCount> _allpassR{};

    // Per-mix-call wet-send accumulator: 2 channels × kMaxFrames.
    std::vector<float> _reverbSendBuffer;
};

} // namespace ayt::audio