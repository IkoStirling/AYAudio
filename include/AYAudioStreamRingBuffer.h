#pragma once
// AYAudioStreamRingBuffer.h - SPSC interleaved F32 ring buffer for streaming PCM
//
// v1.5: a single-producer single-consumer ring buffer that owns the
// in-flight PCM chunks for a streaming voice. The producer (resource /
// loader thread) pushes interleaved float32 frames via write(); the
// consumer (audio thread) drains via read() inside renderVoiceStream().
//
// Storage is preallocated to `capacityFrames * channels` samples. Capacity
// is rounded up to the next power of two so we can mask instead of mod
// for branch-free indexing, mirroring the AudioCommandQueue SPSC pattern.
//
// End-of-stream is signalled lazily: the producer stops pushing frames
// and (optionally) calls markEndOfStream(). The audio thread infers EOS
// when a read returns fewer frames than requested after the ring has been
// drained; the explicit flag is exposed for tests / diagnostics.
//
// Threading contract:
//   - Exactly one producer thread may call write() / markEndOfStream() /
//     close(). Other threads must not call those.
//   - Exactly one consumer thread may call read(). Other threads must not.
//   - close() / isClosed() / endOfStream() / availableRead() /
//     availableWrite() / capacityFrames() / channels() are safe from
//     either side.

#include <AYCore.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace ayt::audio
{

class AudioStreamRingBuffer {
public:
    AudioStreamRingBuffer() = default;
    ~AudioStreamRingBuffer() = default;

    AudioStreamRingBuffer(const AudioStreamRingBuffer&) = delete;
    AudioStreamRingBuffer& operator=(const AudioStreamRingBuffer&) = delete;

    // Configure capacity (rounded up to a power of two) and channel count.
    // Returns false on invalid inputs (capacityFrames == 0, channels == 0).
    // Idempotent: returns true without reallocating if already initialized
    // with matching parameters.
    bool initialize(uint16_t channels, uint32_t capacityFrames);

    // Drops all state. Safe to call multiple times.
    void shutdown();

    // Producer: append `frameCount` interleaved F32 frames from `pcm`.
    // Returns the number of frames actually written (0 when full / closed /
    // channel mismatch). `pcm` must be valid for `frameCount * channels`
    // floats.
    uint32_t write(const float* pcm, uint32_t frameCount);

    // Consumer: pull up to `frameCount` frames into `out` (interleaved
    // float32, size >= `frameCount * channels`). Returns the number of
    // frames actually read; underrun is the EOS signal — caller pads with
    // zeros / marks the voice finished.
    uint32_t read(float* out, uint32_t frameCount);

    // Producer: signal no more frames will arrive. Consumer observes EOS
    // via read() returning 0 frames.
    void markEndOfStream();

    // Producer: stop accepting new writes. Existing readable frames still
    // drain so the consumer can complete fade-out / cleanup. Idempotent.
    void close();

    bool endOfStream() const { return _eos.load(std::memory_order_acquire); }
    bool isClosed() const    { return _closed.load(std::memory_order_acquire); }

    // Approximate counts (safe from either side; not a strict snapshot).
    uint32_t availableRead() const;
    uint32_t availableWrite() const;

    uint32_t capacityFrames() const { return _capacityFrames; }
    uint16_t channels() const       { return _channels; }

private:
    std::vector<float> _buffer;        // size = capacityFrames * channels
    uint32_t _capacityFrames = 0;
    uint16_t _channels = 2;

    // Monotonic uint64 indices; never wrap in any realistic session.
    std::atomic<uint64_t> _writeIndex{0};
    std::atomic<uint64_t> _readIndex{0};
    std::atomic<bool> _eos{false};
    std::atomic<bool> _closed{false};
};

} // namespace ayt::audio
