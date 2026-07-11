#pragma once
// AYAudioClipRegistry.h - Owns PCM blobs and metadata for registered clips
//
// v0 storage: a flat vector indexed by AudioClipId. Clip id 0 is reserved
// (invalid). Ownership of the PCM blob is transferred on registerClip() and
// released on releaseClip() or shutdown().
//
// We store PCM as `std::vector<Byte>` (which owns a contiguous buffer) so
// the registry can return stable pointers for the audio thread. The audio
// thread holds a pointer for at most one render() call; the engine updates
// pointers at command boundaries (after draining the queue).

#include <AYCore.h>
#include <AYAudioTypes.h>
#include <cstdint>
#include <vector>

namespace ayt::audio
{

struct ClipSlot {
    AudioClipDesc desc{};
    std::vector<Byte> storage;          // owned PCM blob
    bool inUse = false;
};

class AudioClipRegistry {
public:
    AudioClipRegistry() = default;

    bool initialize(uint32_t maxClips);

    // Register a clip. Copies `desc.pcmData` into owned storage.
    // Returns InvalidClipId if the registry is full or inputs are invalid.
    AudioClipId registerClip(const AudioClipDesc& desc);

    void releaseClip(AudioClipId id);

    // Returns nullptr for invalid or freed ids.
    const ClipSlot* find(AudioClipId id) const;
    ClipSlot* findMutable(AudioClipId id);

    uint32_t size() const { return _liveCount; }
    uint32_t capacity() const { return _maxClips; }

    void clear();

private:
    std::vector<ClipSlot> _slots;
    uint32_t _maxClips = 0;
    uint32_t _liveCount = 0;       // slots currently in use
};

} // namespace ayt::audio