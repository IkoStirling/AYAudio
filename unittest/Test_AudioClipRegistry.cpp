#include <AYAudio.h>
#include "AYTest.h"

using namespace ayt::audio;

namespace {

// Scratch storage for test clips. Each call to makeSineClip returns a desc
// pointing into a freshly-allocated, engine-local std::vector (owned via
// shared_ptr-like contract: each desc is consumed once by registerClip which
// copies the PCM). Tests must call registerClip before another test overwrites
// the scratch — we keep it simple by reusing one buffer and accepting the
// "last write wins" semantic for tests that reuse the same engine.
std::vector<float> g_scratchBuffer;

AudioClipDesc makeSineClip(uint32_t frameCount, float amplitude = 0.5f,
                           uint16_t channels = 1, AudioSampleFormat fmt = AudioSampleFormat::F32) {
    AudioClipDesc desc{};
    desc.sampleRate = 48000;
    desc.channels = channels;
    desc.format = fmt;
    desc.frameCount = frameCount;
    if (fmt == AudioSampleFormat::F32) {
        g_scratchBuffer.assign(static_cast<size_t>(frameCount) * channels, amplitude);
        desc.pcmData = g_scratchBuffer.data();
    } else {
        static std::vector<int16_t> s16Scratch;
        const int16_t sample = static_cast<int16_t>(amplitude * 32767.0f);
        s16Scratch.assign(static_cast<size_t>(frameCount) * channels, sample);
        desc.pcmData = s16Scratch.data();
    }
    return desc;
}

} // namespace

TEST_SUITE(AudioClipRegistryTests)

    TEST_CASE(InitializeRejectsZero) {
        AudioClipRegistry r;
        CHECK(!r.initialize(0));
        CHECK(r.initialize(4));
        CHECK_INT_EQ(r.capacity(), 4u);
        CHECK_INT_EQ(r.size(), 0u);
    }

    TEST_CASE(RegisterReturnsSequentialIds) {
        AudioClipRegistry r;
        r.initialize(4);
        AudioClipDesc desc = makeSineClip(16);
        AudioClipId a = r.registerClip(desc);
        AudioClipId b = r.registerClip(desc);
        CHECK(a != InvalidClipId);
        CHECK(b != InvalidClipId);
        CHECK(b == a + 1);
        r.clear();
    }

    TEST_CASE(RegisterRejectsInvalidInput) {
        AudioClipRegistry r;
        r.initialize(4);
        AudioClipDesc bad{};

        bad.frameCount = 0;
        bad.pcmData = nullptr;
        CHECK(r.registerClip(bad) == InvalidClipId);

        bad = makeSineClip(8);
        bad.channels = 5; // not 1 or 2
        CHECK(r.registerClip(bad) == InvalidClipId);

        bad = makeSineClip(8);
        bad.format = static_cast<AudioSampleFormat>(99);
        CHECK(r.registerClip(bad) == InvalidClipId);
    }

    TEST_CASE(RegisterRejectsWhenFull) {
        AudioClipRegistry r;
        r.initialize(2);
        AudioClipDesc desc = makeSineClip(8);
        CHECK(r.registerClip(desc) != InvalidClipId);
        CHECK(r.registerClip(desc) != InvalidClipId);
        CHECK(r.registerClip(desc) == InvalidClipId);
        r.clear();
    }

    TEST_CASE(ReleaseClipClearsStorage) {
        AudioClipRegistry r;
        r.initialize(4);
        AudioClipDesc desc = makeSineClip(8);
        AudioClipId id = r.registerClip(desc);
        CHECK(r.find(id) != nullptr);
        r.releaseClip(id);
        CHECK(r.find(id) == nullptr);
    }

    TEST_CASE(ReleasedSlotReusedForNewClip) {
        AudioClipRegistry r;
        r.initialize(2);
        AudioClipDesc desc = makeSineClip(8);
        AudioClipId a = r.registerClip(desc);
        r.releaseClip(a);
        AudioClipId b = r.registerClip(desc);
        // After release + register, the freed slot is reused → same id.
        CHECK(a == b);
        r.clear();
    }

    TEST_CASE(FindReturnsNullForInvalidId) {
        AudioClipRegistry r;
        r.initialize(2);
        CHECK(r.find(InvalidClipId) == nullptr);
        CHECK(r.find(999) == nullptr);
    }

    TEST_CASE(StoredPcmIsIndependentOfCallerBuffer) {
        AudioClipRegistry r;
        r.initialize(2);
        AudioClipDesc desc = makeSineClip(8, 0.25f);
        const AudioClipId id = r.registerClip(desc);
        const ClipSlot* slot = r.find(id);
        CHECK(slot != nullptr);
        CHECK(slot->desc.pcmData != desc.pcmData); // registry owns its copy
        // Wipe the caller's scratch buffer; registry's copy must survive.
        g_scratchBuffer.assign(g_scratchBuffer.size(), 0.0f);
        const float* stored = reinterpret_cast<const float*>(slot->desc.pcmData);
        CHECK_FLOAT_EQ(stored[0], 0.25f, 1e-6f);
        r.clear();
    }

    TEST_CASE(ClearResetsAllState) {
        AudioClipRegistry r;
        r.initialize(4);
        AudioClipDesc desc = makeSineClip(8);
        const AudioClipId a = r.registerClip(desc);
        const AudioClipId b = r.registerClip(desc);
        r.clear();
        CHECK(r.find(a) == nullptr);
        CHECK(r.find(b) == nullptr);
        // Re-register after clear should succeed and reuse id 1.
        const AudioClipId c = r.registerClip(desc);
        CHECK(c == a);
    }    
TEST_SUITE_END
