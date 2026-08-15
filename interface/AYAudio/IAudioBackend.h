#pragma once
// AYAudio/IAudioBackend.h - Audio device backend abstraction
//
// AYAudio (v0.5) uses miniaudio as the primary device backend. This interface
// isolates the engine core from any particular audio API so:
//   - MockBackend   drives unit tests without a sound card
//   - NullBackend   is wired when -no-audio is set
//   - MiniaudioBackend produces interleaved float32 in a callback thread
//
// All methods are called from the audio thread (backend-owned) or from the
// engine mixer (CPU side); see AYAudioEngine for ownership.

#include <cstddef>
#include <cstdint>

namespace ayt::audio
{

struct AudioBackendInfo {
    const char* name = nullptr;     // "Mock" / "Null" / "Miniaudio"
    uint32_t sampleRate = 0;        // device sample rate (Hz)
    uint16_t channels = 0;          // 1=mono, 2=stereo (interleaved)
    uint16_t bitsPerSample = 0;     // 16 or 32
    bool realDevice = false;        // true if backend opens real hardware
};

class AudioEngine;

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // Open the device and prepare to produce frames.
    // sampleRate, channels, format must match what AYAudioEngine expects.
    // Returns true on success.
    virtual bool start(const AudioBackendInfo& info) = 0;

    // Stop the device and release any native resources.
    virtual void stop() = 0;

    // Pull `frameCount` interleaved float32 samples (length = frameCount * channels).
    // Called from audio thread; must be lock-free relative to engine commands.
    virtual void render(float* output, uint32_t frameCount, uint32_t channels) = 0;

    // Diagnostic snapshot.
    virtual AudioBackendInfo describe() const = 0;

    // True when backend actually opened a hardware device.
    virtual bool isRealDevice() const = 0;

    // Optional: real-device backends that pull frames from AudioEngine in a
    // device callback override this. Default is a no-op (Null/Mock).
    virtual void bindAudioEngine(AudioEngine* /*engine*/) {}
};

} // namespace ayt::audio