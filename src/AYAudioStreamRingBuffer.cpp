#include <AYAudio/AudioStreamRingBuffer.h>

#include <algorithm>
#include <cstring>

namespace ayt::audio
{

namespace {

constexpr bool isPowerOfTwo(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

uint32_t nextPowerOfTwo(uint32_t v) {
    if (v < 2) return 2;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

} // namespace

bool AudioStreamRingBuffer::initialize(uint16_t channels, uint32_t capacityFrames) {
    if (channels == 0 || capacityFrames == 0) {
        return false;
    }
    // Idempotent: keep existing storage if parameters match.
    if (_capacityFrames != 0 && _channels == channels && _capacityFrames == capacityFrames) {
        return true;
    }
    if (_capacityFrames != 0) {
        // Different parameters — discard old storage. Producer/consumer are
        // not expected to call initialize() while traffic is flowing.
        shutdown();
    }

    if (!isPowerOfTwo(capacityFrames)) {
        capacityFrames = nextPowerOfTwo(capacityFrames);
    }
    _channels = channels;
    _capacityFrames = capacityFrames;
    _buffer.assign(static_cast<size_t>(capacityFrames) * channels, 0.0f);

    _writeIndex.store(0, std::memory_order_relaxed);
    _readIndex.store(0, std::memory_order_relaxed);
    _eos.store(false, std::memory_order_relaxed);
    _closed.store(false, std::memory_order_relaxed);
    return true;
}

void AudioStreamRingBuffer::shutdown() {
    _buffer.clear();
    _buffer.shrink_to_fit();
    _capacityFrames = 0;
    _channels = 2;
    _writeIndex.store(0, std::memory_order_relaxed);
    _readIndex.store(0, std::memory_order_relaxed);
    _eos.store(false, std::memory_order_relaxed);
    _closed.store(false, std::memory_order_relaxed);
}

uint32_t AudioStreamRingBuffer::write(const float* pcm, uint32_t frameCount) {
    if (_capacityFrames == 0 || pcm == nullptr || frameCount == 0) {
        return 0;
    }
    if (_closed.load(std::memory_order_acquire)) {
        return 0;
    }

    const uint64_t writeIdx = _writeIndex.load(std::memory_order_relaxed);
    const uint64_t readIdx  = _readIndex.load(std::memory_order_acquire);

    const uint64_t inFlight = writeIdx - readIdx;
    if (inFlight >= _capacityFrames) {
        return 0; // full
    }
    const uint32_t freeFrames = static_cast<uint32_t>(_capacityFrames - inFlight);
    const uint32_t toWrite = std::min(frameCount, freeFrames);
    if (toWrite == 0) {
        return 0;
    }

    const uint32_t mask = _capacityFrames - 1;
    const uint32_t writeOffset = static_cast<uint32_t>(writeIdx & mask);
    float* dst = _buffer.data() + static_cast<size_t>(writeOffset) * _channels;

    if (writeOffset + toWrite <= _capacityFrames) {
        // Single contiguous copy.
        std::memcpy(dst, pcm, static_cast<size_t>(toWrite) * _channels * sizeof(float));
    } else {
        // Wrap-around: tail first, then head.
        const uint32_t first = _capacityFrames - writeOffset;
        std::memcpy(dst, pcm, static_cast<size_t>(first) * _channels * sizeof(float));
        const uint32_t second = toWrite - first;
        std::memcpy(_buffer.data(),
                    pcm + static_cast<size_t>(first) * _channels,
                    static_cast<size_t>(second) * _channels * sizeof(float));
    }

    // Release-store the new write index AFTER the bytes are committed.
    // This pairs with the consumer's acquire-load: the consumer is
    // guaranteed to see the freshly-written samples before observing
    // the updated index.
    _writeIndex.store(writeIdx + toWrite, std::memory_order_release);
    return toWrite;
}

uint32_t AudioStreamRingBuffer::read(float* out, uint32_t frameCount) {
    if (_capacityFrames == 0 || out == nullptr || frameCount == 0) {
        return 0;
    }

    const uint64_t readIdx  = _readIndex.load(std::memory_order_relaxed);
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_acquire);

    const uint64_t available = writeIdx - readIdx;
    if (available == 0) {
        return 0;
    }
    const uint32_t availFrames = static_cast<uint32_t>(available);
    const uint32_t toRead = std::min(frameCount, availFrames);
    if (toRead == 0) {
        return 0;
    }

    const uint32_t mask = _capacityFrames - 1;
    const uint32_t readOffset = static_cast<uint32_t>(readIdx & mask);
    const float* src = _buffer.data() + static_cast<size_t>(readOffset) * _channels;

    if (readOffset + toRead <= _capacityFrames) {
        std::memcpy(out, src, static_cast<size_t>(toRead) * _channels * sizeof(float));
    } else {
        const uint32_t first = _capacityFrames - readOffset;
        std::memcpy(out, src, static_cast<size_t>(first) * _channels * sizeof(float));
        const uint32_t second = toRead - first;
        std::memcpy(out + static_cast<size_t>(first) * _channels,
                    _buffer.data(),
                    static_cast<size_t>(second) * _channels * sizeof(float));
    }

    _readIndex.store(readIdx + toRead, std::memory_order_release);
    return toRead;
}

void AudioStreamRingBuffer::markEndOfStream() {
    _eos.store(true, std::memory_order_release);
}

void AudioStreamRingBuffer::close() {
    // Closing only blocks future writes; existing readable frames still
    // drain so the consumer can fade out cleanly.
    _closed.store(true, std::memory_order_release);
}

uint32_t AudioStreamRingBuffer::availableRead() const {
    if (_capacityFrames == 0) return 0;
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_acquire);
    const uint64_t readIdx  = _readIndex.load(std::memory_order_acquire);
    if (writeIdx < readIdx) return 0;
    const uint64_t inFlight = writeIdx - readIdx;
    return inFlight >= _capacityFrames
               ? _capacityFrames
               : static_cast<uint32_t>(inFlight);
}

uint32_t AudioStreamRingBuffer::availableWrite() const {
    if (_capacityFrames == 0) return 0;
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_acquire);
    const uint64_t readIdx  = _readIndex.load(std::memory_order_acquire);
    if (writeIdx < readIdx) return 0;
    const uint64_t inFlight = writeIdx - readIdx;
    if (inFlight >= _capacityFrames) return 0;
    return static_cast<uint32_t>(_capacityFrames - inFlight);
}

} // namespace ayt::audio
