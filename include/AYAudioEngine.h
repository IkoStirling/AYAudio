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

class AudioStreamRingBuffer; // full type lives in src/AYAudioEngine.cpp

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

    // ===== Streaming PCM sources (v1.5) =====
    //
    // The producer (resource / loader thread) pre-registers a streaming
    // handle, then asynchronously pushes interleaved F32 frames into the
    // underlying SPSC ring. Voices created with playStream() drain from
    // this ring on the audio thread.
    //
    // openStream / closeStream take the same dual-write path as
    // registerClip / releaseClip (push command + synchronous apply).
    // streamPush / playStream use direct mutation + voice allocate,
    // matching play() / setVoiceX() patterns.

    AudioStreamId openStream(const AudioStreamDesc& desc);
    void closeStream(AudioStreamId id);

    // Producer-side push. Returns frames actually written (0 when full /
    // closed / channel mismatch). Safe from the producer thread only.
    uint32_t streamPush(AudioStreamId id, const float* pcm, uint32_t frameCount);

    // Start a voice that drains from a streaming ring buffer.
    VoiceHandle playStream(AudioStreamId stream,
                           AudioBus bus = AudioBus::Sfx,
                           const SpatializationParams& spatial = {},
                           float volume = 1.0f,
                           bool loop = false);

    // ===== Engine-wide pitch multiplier (v1.5) =====
    //
    // Applied to every active voice on the audio thread (`voice.pitch *
    // timeScale`). Range clamped to [0.25, 4.0]. Mutated directly on the
    // game thread — only `render()` reads it, so no command-queue pass
    // (mirrors `_paused`).
    void setTimeScale(float scale);
    float timeScale() const { return _timeScale; }

    // Diagnostic accessors for tests.
    AudioStreamRingBuffer* streamRing(AudioStreamId id);
    const AudioStreamDesc& streamDesc(AudioStreamId id) const;

    // ===== v2.0b engine-side audio effects =====

    // Configure ducking (Music bus sidechain). `depthDb` ≤ 0 (negative = duck).
    // `attackPerSec` and `releasePerSec` are exponential ramp rates in dB/s.
    // depthDb == 0 disables the sidechain.
    void setDucking(float depthDb, float attackPerSec, float releasePerSec);

    // Master wet-level for the reverb send bus. 0..1 linear.
    void setMasterReverbReturn(float linearReturn);

    // Select the Schroeder reverb preset (0 = small room, 1 = hall).
    // Out-of-range values clamp to preset 0.
    void setReverbPreset(uint8_t preset);

    // Multiplier on the doppler pitch factor computed per voice. 0 disables
    // doppler; 1 is the engine default. Clamped to [0, 2].
    void setDopplerScale(float scale);
    float dopplerScale() const { return _dopplerScale; }

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
    VoiceHandle _allocateVoiceIndex(uint32_t slotIdx);

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

    // v1.5: fixed 8-slot streaming pool. Each slot owns a ring buffer; the
    // game-thread producer writes via streamPush(); the audio-thread consumer
    // reads inside renderVoiceStream().
    struct StreamSlot {
        AudioStreamDesc desc{};
        std::unique_ptr<AudioStreamRingBuffer> ring;
        bool inUse = false;
    };
    static constexpr uint32_t kMaxStreams = 8;
    std::array<StreamSlot, kMaxStreams> _streams{};

    // Engine-wide pitch multiplier. Clamped to [0.25, 4.0] in setTimeScale.
    // Consumed (read) on the audio thread inside the per-voice source
    // callbacks — game-thread writes are atomic because float writes are
    // naturally aligned and the audio thread reads once per voice per
    // render block; a torn read only affects a single block boundary.
    float _timeScale = 1.0f;

    // v2.0b: doppler scale multiplier. Same single-audio-thread / once-per-
    // block snapshot pattern as _timeScale. Clamped to [0, 2].
    float _dopplerScale = 1.0f;
};

} // namespace ayt::audio