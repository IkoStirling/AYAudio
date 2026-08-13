#include <AYAudio.h>
#include "AYTest.h"

#include <cmath>
#include <vector>

using namespace ayt::audio;

namespace {

AudioSettings settings48k() {
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 32;
    s.maxVoices = 8;
    s.maxClips = 8;
    return s;
}

} // namespace

TEST_SUITE(AudioSampleRatePolicyTests)

    TEST_CASE(RegisterClipResamplesS16From22050To48000) {
        AudioEngine engine;
        CHECK(engine.initialize(settings48k()));

        // 4 frames @ 22050 mono → ~9 frames @ 48000 (4 * 48000/22050 ≈ 8.7).
        constexpr uint32_t kSrcFrames = 4;
        std::vector<int16_t> pcm(kSrcFrames, 1000);
        AudioClipDesc desc{};
        desc.sampleRate = 22050;
        desc.channels = 1;
        desc.format = AudioSampleFormat::S16;
        desc.bitsPerSample = 16;
        desc.frameCount = kSrcFrames;
        desc.pcmData = pcm.data();

        const AudioClipId id = engine.registerClip(desc);
        CHECK(id != InvalidClipId);

        const ClipSlot* slot = engine.clipRegistry().find(id);
        CHECK(slot != nullptr);
        CHECK_INT_EQ(static_cast<int>(slot->desc.sampleRate), 48000);
        CHECK(slot->desc.frameCount >= 8u);
        CHECK(slot->desc.frameCount <= 10u);
        engine.shutdown();
    }

    TEST_CASE(RegisterClipSameRateIsNoOpCopy) {
        AudioEngine engine;
        CHECK(engine.initialize(settings48k()));

        std::vector<float> pcm(16, 0.25f);
        AudioClipDesc desc{};
        desc.sampleRate = 48000;
        desc.channels = 1;
        desc.format = AudioSampleFormat::F32;
        desc.frameCount = 16;
        desc.pcmData = pcm.data();

        const AudioClipId id = engine.registerClip(desc);
        CHECK(id != InvalidClipId);
        const ClipSlot* slot = engine.clipRegistry().find(id);
        CHECK(slot != nullptr);
        CHECK_INT_EQ(static_cast<int>(slot->desc.sampleRate), 48000);
        CHECK_INT_EQ(static_cast<int>(slot->desc.frameCount), 16);
        engine.shutdown();
    }

TEST_SUITE_END

TEST_SUITE(AudioHrtfTests)

    TEST_CASE(HrtfEnabledWidensIldVsEqualPower) {
        ListenerParams listener{};
        listener.position = {0, 0, 0};
        listener.forward = {0, 0, -1};
        listener.up = {0, 1, 0};
        listener.hrtfEnabled = false;

        SpatializationParams voice{};
        voice.mode = SpatializationParams::Mode::World;
        voice.spatialBlend = 1.0f;
        voice.attenuation = SpatializationParams::AttenuationModel::Linear;
        voice.referenceDistance = 1.0f;
        voice.maxDistance = 100.0f;
        voice.position = {5.0f, 0.0f, 0.0f}; // hard right

        SpatialGains plain = AudioSpatializer::compute(listener, voice, 2);
        listener.hrtfEnabled = true;
        SpatialGains hrtf = AudioSpatializer::compute(listener, voice, 2);

        // Right-side source: HRTF increases ILD (R/L ratio) vs equal-power alone.
        const float plainIld = plain.rightGain / (plain.leftGain + 1e-6f);
        const float hrtfIld = hrtf.rightGain / (hrtf.leftGain + 1e-6f);
        CHECK(hrtfIld > plainIld);
        CHECK(hrtf.rightGain > hrtf.leftGain);
    }

TEST_SUITE_END
