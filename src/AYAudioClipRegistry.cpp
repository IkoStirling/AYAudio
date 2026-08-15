#include <AYAudio/AudioClipRegistry.h>

#include <cstring>

namespace ayt::audio
{

bool AudioClipRegistry::initialize(uint32_t maxClips) {
    if (maxClips == 0) {
        return false;
    }
    _maxClips = maxClips;
    _slots.assign(maxClips, ClipSlot{});
    _liveCount = 0;
    return true;
}

void AudioClipRegistry::clear() {
    for (auto& slot : _slots) {
        slot.storage.clear();
        slot.storage.shrink_to_fit();
        slot.inUse = false;
        slot.desc = AudioClipDesc{};
    }
    _liveCount = 0;
}

namespace {

uint32_t bytesPerSample(AudioSampleFormat fmt) {
    switch (fmt) {
        case AudioSampleFormat::S16: return 2;
        case AudioSampleFormat::F32: return 4;
    }
    return 0;
}

} // namespace

AudioClipId AudioClipRegistry::registerClip(const AudioClipDesc& desc) {
    if (desc.frameCount == 0 || desc.pcmData == nullptr) {
        return InvalidClipId;
    }
    if (desc.channels != 1 && desc.channels != 2) {
        return InvalidClipId;
    }
    const uint32_t bps = bytesPerSample(desc.format);
    if (bps == 0) {
        return InvalidClipId;
    }

    // Find an empty slot.
    int32_t slotIndex = -1;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        if (!_slots[i].inUse) {
            slotIndex = static_cast<int32_t>(i);
            break;
        }
    }
    if (slotIndex < 0) {
        return InvalidClipId;
    }

    ClipSlot& slot = _slots[slotIndex];
    slot.desc = desc;
    const size_t byteCount = static_cast<size_t>(desc.frameCount) * desc.channels * bps;
    slot.storage.resize(byteCount);
    std::memcpy(slot.storage.data(), desc.pcmData, byteCount);
    slot.desc.pcmData = slot.storage.data(); // point at our owned copy
    slot.inUse = true;
    ++_liveCount;

    // id == slotIndex + 1 keeps releaseClip(id) and find(id) O(1) and lets
    // us reuse a freed slot's id after releaseClip.
    const AudioClipId id = static_cast<AudioClipId>(slotIndex) + 1;
    return id;
}

void AudioClipRegistry::releaseClip(AudioClipId id) {
    if (id == InvalidClipId) {
        return;
    }
    // Slot index = id - 1 (id 1 → slot 0, id N → slot N-1). We trust the
    // engine to only pass ids that were returned from registerClip().
    const uint32_t idx = id - 1;
    if (idx >= _slots.size()) {
        return;
    }
    ClipSlot& slot = _slots[idx];
    if (!slot.inUse) {
        return;
    }
    slot.storage.clear();
    slot.storage.shrink_to_fit();
    slot.desc = AudioClipDesc{};
    slot.inUse = false;
    --_liveCount;
}

const ClipSlot* AudioClipRegistry::find(AudioClipId id) const {
    if (id == InvalidClipId) {
        return nullptr;
    }
    const uint32_t idx = id - 1;
    if (idx >= _slots.size() || !_slots[idx].inUse) {
        return nullptr;
    }
    return &_slots[idx];
}

ClipSlot* AudioClipRegistry::findMutable(AudioClipId id) {
    if (id == InvalidClipId) {
        return nullptr;
    }
    const uint32_t idx = id - 1;
    if (idx >= _slots.size() || !_slots[idx].inUse) {
        return nullptr;
    }
    return &_slots[idx];
}

} // namespace ayt::audio