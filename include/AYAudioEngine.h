#pragma once
// AYAudioEngine.h - Runtime presentation audio engine (public API)
//
// AYAudioEngine is the single entry point gameplay code uses to drive audio.
// It owns the command queue, mixer, spatializer, and clip registry, and
// drives the configured IAudioBackend.
//
// Lifecycle:
//   1. Construct engine (default ctor is fine).
//   2. initialize(settings) — picks a backend and brings it up.
//   3. From the game thread, call play/stop/setListener/etc.
//   4. From the game thread each frame, call submitFrame(dt).
//   5. shutdown() — stops voices, closes backend, releases state.
//
// Threading:
//   - All public methods (except render() and the backend itself) are
//     intended for the game thread.
//   - The audio thread (MiniaudioBackend::render callback in v0.5) calls
//     AudioEngine::render() — that path is public so backends can plug in
//     but must only be used by the backend's render callback.

#include <AYCore.h>
#include <AYAudioTypes.h>
#include <AYAudioCommandQueue.h>
#include <AYAudioMixer.h>
#include <AYAudioSpatializer.h>
#include <AYAudioClipRegistry.h>

#include <IAudioBackend.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ayt::audio
{

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ===== Lifecycle =====

    bool initialize(const AudioSettings& settings);
    void shutdown();

    bool isInitialized() const { return _initialized; }

    // ===== Clip registration =====

    AudioClipId registerClip(const AudioClipDesc& desc);
    void releaseClip(AudioClipId id);

    // ===== Playback control =====

    VoiceHandle play(AudioClipId clip,
                     AudioBus bus = AudioBus::Sfx,
                     const SpatializationParams& spatial = {},
                     float volume = 1.0f,
                     bool loop = false);

    void stop(VoiceHandle voice);
    void stopAll();

    void setVoiceVolume(VoiceHandle voice, float volume);
    void setVoicePitch(VoiceHandle voice, float pitch);
    void setVoiceSpatial(VoiceHandle voice, const SpatializationParams& spatial);

    // ===== Bus / master =====

    void setBusGain(AudioBus bus, float linearGain);
    void setMasterGain(float linearGain);

    // ===== Listener =====

    void setListener(const ListenerParams& listener);

    // ===== Pause / resume (drives a flag checked in render) =====

    void pause();
    void resume();
    bool isPaused() const { return _paused; }

    // ===== Per-frame tick (game thread) =====

    // Drains pending commands and applies state on the audio side. Cheap;
    // call once per game frame before the backend renders. deltaTime is
    // reserved for future use (smooth doppler ramps).
    void submitFrame(float deltaTime);

    // ===== Backend integration =====

    // Pull the next audio buffer (interleaved float32). Called by the
    // backend's data callback (audio thread). Safe to call from any thread
    // because it operates on the audio-thread-owned state copy after the
    // command queue has been drained.
    void render(float* output, uint32_t frameCount);

    // Backend introspection (used by AudioSubSystem).
    IAudioBackend* backend() const { return _backend.get(); }
    const AudioBackendInfo& backendInfo() const { return _backendInfo; }
    uint16_t outputChannels() const { return _settings.channels; }
    uint32_t outputSampleRate() const { return _settings.sampleRate; }

    // Install a backend before initialize(). The engine takes ownership.
    // Calling this after initialize() is a no-op (hot-swap not in v0).
    void setBackend(std::unique_ptr<IAudioBackend> backend);

    // Direct accessors for tests / diagnostics.
    const AudioSettings& settings() const { return _settings; }
    AudioClipRegistry& clipRegistry() { return _clips; }
    AudioMixer& mixer() { return _mixer; }
    AudioCommandQueue& commandQueue() { return _commands; }
    const ListenerParams& listener() const { return _listener; }

    // Voice iteration helpers (for tests and editor).
    uint32_t activeVoiceCount() const;
    const VoiceRuntime* voiceAt(uint32_t index) const;

private:
    // ===== Internal helpers =====
    VoiceHandle _allocateVoice();
    VoiceHandle _findVoiceSlot(AudioClipId clip, bool playingOnly) const;

    void _applyCommand(const AudioCommand& cmd);
    void _resolveVoice(AudioClipId clip,
                       AudioBus bus,
                       const SpatializationParams& spatial,
                       float volume,
                       bool loop,
                       VoiceHandle& outVoice);

    static void _voiceSource(void* userData,
                             uint32_t voiceIndex,
                             float* voiceBuffer,
                             uint32_t frameCount);

    // ===== State =====

    AudioSettings _settings{};
    AudioBackendInfo _backendInfo{};
    ListenerParams _listener{};

    AudioCommandQueue _commands;
    AudioClipRegistry _clips;
    AudioMixer _mixer;

    std::unique_ptr<IAudioBackend> _backend;

    // Voice pool. Index 0 is unused so VoiceHandle == index + 1.
    std::vector<VoiceRuntime> _voices;
    uint32_t _voiceCapacity = 0;
    uint32_t _nextVoiceId = 1;          // monotonic, allows gaps after stop
    uint64_t _engineFrame = 0;          // ticks on every submitFrame

    bool _initialized = false;
    bool _paused = false;
};

} // namespace ayt::audio