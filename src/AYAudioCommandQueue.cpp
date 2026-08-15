#include <AYAudio/AudioCommandQueue.h>

#include <cassert>

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

bool AudioCommandQueue::initialize(uint32_t capacity) {
    if (_buffer.capacity() != 0) {
        return true; // idempotent: already initialized
    }
    if (capacity == 0) {
        return false;
    }
    if (!isPowerOfTwo(capacity)) {
        capacity = nextPowerOfTwo(capacity);
    }
    _capacity = capacity;
    _mask = capacity - 1;
    _buffer.resize(_capacity);
    _writeIndex.store(0, std::memory_order_relaxed);
    _readIndex.store(0, std::memory_order_relaxed);
    return true;
}

void AudioCommandQueue::shutdown() {
    _buffer.clear();
    _buffer.shrink_to_fit();
    _capacity = 0;
    _mask = 0;
    _writeIndex.store(0, std::memory_order_relaxed);
    _readIndex.store(0, std::memory_order_relaxed);
}

bool AudioCommandQueue::tryPush(const AudioCommand& cmd) {
    if (_buffer.empty()) {
        return false;
    }
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_relaxed);
    const uint64_t readIdx  = _readIndex.load(std::memory_order_acquire);

    if (writeIdx - readIdx >= _capacity) {
        return false; // full
    }

    _buffer[static_cast<size_t>(writeIdx & _mask)] = cmd;
    _writeIndex.store(writeIdx + 1, std::memory_order_release);
    return true;
}

bool AudioCommandQueue::tryPop(AudioCommand& outCmd) {
    if (_buffer.empty()) {
        return false;
    }
    const uint64_t readIdx  = _readIndex.load(std::memory_order_relaxed);
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_acquire);

    if (readIdx == writeIdx) {
        return false; // empty
    }

    outCmd = _buffer[static_cast<size_t>(readIdx & _mask)];
    _readIndex.store(readIdx + 1, std::memory_order_release);
    return true;
}

uint32_t AudioCommandQueue::approximateDepth() const {
    const uint64_t writeIdx = _writeIndex.load(std::memory_order_relaxed);
    const uint64_t readIdx  = _readIndex.load(std::memory_order_relaxed);
    if (writeIdx < readIdx) {
        return 0;
    }
    const uint64_t depth = writeIdx - readIdx;
    return depth > _capacity ? _capacity : static_cast<uint32_t>(depth);
}

} // namespace ayt::audio