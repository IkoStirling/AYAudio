#pragma once
// AYAudioCommandQueue.h - Lock-free SPSC command queue (game→audio thread)
//
// v0 implementation: single-producer single-consumer ring buffer of
// AudioCommand records. The producer is the game thread (calls in
// AudioEngine::play/stop/etc); the consumer is the audio thread (drains
// inside AudioEngine::render()).
//
// Capacity is rounded up to a power of two; we mask instead of modding for
// branch-free indexing. Memory is allocated once at startup and reused.
//
// This is intentionally simple — no priority lanes, no fan-out. The design
// reserves this module for future extension (priority stealing, etc.) but
// keeps the surface small for v0 review.

#include <AYCore.h>
#include <AYAudioTypes.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ayt::audio
{

enum class AudioCommandType : uint8_t {
    Play,
    Stop,
    StopAll,
    SetVoiceVolume,
    SetVoicePitch,
    SetVoiceSpatialParams,
    SetBusGain,
    SetMasterGain,
    SetListener,
    RegisterClip,
    ReleaseClip,
    PauseEngine,
    ResumeEngine,
    // v1.5: streaming source lifecycle.
    OpenStream,     // payload: streamDesc
    CloseStream,    // payload: stream
    PlayStream,     // payload: stream + bus/spatial/volume/loop
    // v2.0b: engine-side audio effects.
    SetDucking,        // scalar=depthDb, scalar2=attackPerSec
    SetReverbReturn,   // scalar=linearReturn
    SetReverbPreset,   // uint8_t preset (repacked into boolFlag)
    SetDopplerScale,   // scalar=scale [0..2]
};

// POD-style command. All data lives inline so the queue is a flat array.
// We pack carefully to keep each record small (<= 80 bytes is plenty for v0).

struct AudioCommand {
    AudioCommandType type = AudioCommandType::Play;

    // Scalar payload (meaning depends on `type`).
    AudioClipId clip = InvalidClipId;
    VoiceHandle voice = InvalidVoice;
    AudioBus bus = AudioBus::Sfx;
    float scalar  = 0.0f;        // volume, pitch, gain, etc.
    float scalar2 = 0.0f;        // v2.0b: secondary scalar (e.g. ducking attack)
    float scalar3 = 0.0f;        // v2.0b: tertiary scalar (e.g. ducking release)
    uint8_t loop = 0;
    uint8_t boolFlag = 0;

    // Aggregate payload (used by Play/SetVoiceSpatialParams/RegisterClip/SetListener).
    SpatializationParams spatial{};
    AudioClipDesc clipDesc{};
    ListenerParams listener{};

    // v1.5 streaming payload.
    AudioStreamDesc streamDesc{}; // OpenStream
    AudioStreamId   stream = InvalidStreamId; // CloseStream / PlayStream
};

class AudioCommandQueue {
public:
    AudioCommandQueue() = default;
    ~AudioCommandQueue() = default;

    // Allocates the ring buffer. capacity is rounded up to the next power of two.
    bool initialize(uint32_t capacity);

    void shutdown();

    // Game thread: try to push. Returns false if the queue is full (caller
    // decides whether to log/drop/retry).
    bool tryPush(const AudioCommand& cmd);

    // Audio thread: pop the next command. Returns false when the queue is
    // empty. Safe to call from a single consumer thread only.
    bool tryPop(AudioCommand& outCmd);

    // Snapshot of current depth (approximate; safe to read from either side).
    uint32_t approximateDepth() const;

    uint32_t capacity() const { return _capacity; }
    bool isInitialized() const { return _capacity != 0; }

private:
    // Power-of-two helpers and storage. _buffer holds _capacity AudioCommands
    // laid out contiguously. We store AudioCommand by value to keep pushes
    // trivially copyable.
    std::vector<AudioCommand> _buffer;
    uint32_t _capacity = 0;
    uint32_t _mask = 0;

    // Index of next slot to write (producer) / read (consumer). Monotonic
    // uint64 so we never wrap inside a realistic session.
    std::atomic<uint64_t> _writeIndex{0};
    std::atomic<uint64_t> _readIndex{0};
};

} // namespace ayt::audio