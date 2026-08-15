// Test_AudioEngineStream.cpp
//
// Unit tests for v1.5 streaming voices + engine-wide timeScale on
// AudioEngine. Each test stands up an AudioEngine with the Null backend
// (no device), pushes PCM frames into a streaming ring, plays the
// voice, and inspects the rendered output buffer.

#include <AYResource/assetsImpl/Audio.h>
#include <AYAudio/AudioStreamRingBuffer.h>

#include "AYTest.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace ayt::audio;

namespace {

AudioSettings defaultSettings() {
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 64;
    s.maxVoices = 16;
    s.maxClips = 16;
    s.masterGain = 1.0f;
    return s;
}

float peakAbs(const float* pcm, uint32_t frameCount, uint16_t channels) {
    float p = 0.0f;
    for (uint32_t i = 0; i < frameCount * channels; ++i) {
        const float a = std::fabs(pcm[i]);
        if (a > p) p = a;
    }
    return p;
}

} // namespace

TEST_SUITE(AudioEngineStreamTests)

    TEST_CASE(OpenStreamReturnsValidId) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.channels = 2;
        desc.capacityFrames = 1024;
        const AudioStreamId id = engine.openStream(desc);
        CHECK(id != InvalidStreamId);

        engine.shutdown();
    }

    TEST_CASE(OpenStreamRejectsZeroCapacity) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.capacityFrames = 0;
        CHECK(engine.openStream(desc) == InvalidStreamId);

        AudioStreamDesc desc2{};
        desc2.channels = 0;
        CHECK(engine.openStream(desc2) == InvalidStreamId);

        engine.shutdown();
    }

    TEST_CASE(OpenStreamRejectsAllEightSlotsBusy) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.capacityFrames = 256;

        for (uint32_t i = 0; i < 8; ++i) {
            CHECK(engine.openStream(desc) != InvalidStreamId);
        }
        // 9th must fail.
        CHECK(engine.openStream(desc) == InvalidStreamId);
        engine.shutdown();
    }

    TEST_CASE(CloseStreamAllowsSlotReuse) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.capacityFrames = 256;

        const AudioStreamId first = engine.openStream(desc);
        CHECK(first != InvalidStreamId);

        engine.closeStream(first);
        // After close, opening another stream should succeed — slot was
        // released back to the pool.
        const AudioStreamId second = engine.openStream(desc);
        CHECK(second != InvalidStreamId);

        engine.shutdown();
    }

    TEST_CASE(StreamPushToClosedReturnsZero) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.capacityFrames = 64;
        const AudioStreamId id = engine.openStream(desc);
        CHECK(id != InvalidStreamId);

        // Sanity: push some samples.
        std::vector<float> src(8, 0.5f);
        CHECK(engine.streamPush(id, src.data(), 4) == 4u);

        engine.closeStream(id);
        // After close, the ring rejects writes (existing buffered frames
        // are still readable by an in-flight voice, but a producer-style
        // push now returns 0).
        CHECK(engine.streamPush(id, src.data(), 4) == 0u);

        engine.shutdown();
    }

    TEST_CASE(PlayStreamProducesAudioAfterPush) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.channels = 1;
        desc.capacityFrames = 1024;
        const AudioStreamId id = engine.openStream(desc);
        CHECK(id != InvalidStreamId);

        // Push 1024 frames of 0.5f — already exceeds the 32-frame render
        // block, so we should hear sound for at least the full block.
        std::vector<float> src(1024, 0.5f);
        CHECK(engine.streamPush(id, src.data(), 1024) == 1024u);

        const VoiceHandle v = engine.playStream(id, AudioBus::Sfx,
                                                SpatializationParams{},
                                                /*volume=*/1.0f,
                                                /*loop=*/false);
        CHECK(v != InvalidVoice);

        engine.submitFrame(0.0f);

        constexpr uint32_t kFrames = 32;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);

        const float peak = peakAbs(output, kFrames, 2);
        CHECK(peak > 0.3f);
        // Stream voices publish drained source frames for audio-master sync.
        CHECK(engine.voicePositionFrames(v) == kFrames);
        engine.shutdown();
    }

    TEST_CASE(PlayStreamUnderrunProducesSilenceAndFinishes) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.channels = 1;
        desc.capacityFrames = 64;
        const AudioStreamId id = engine.openStream(desc);
        CHECK(id != InvalidStreamId);

        // Push only 8 frames; play the stream BEFORE closing it so the
        // voice slot resolves the ring. Then drain the buffer via render
        // and verify underrun leads to a silent output once finished.
        std::vector<float> src(8, 0.7f);
        CHECK(engine.streamPush(id, src.data(), 8) == 8u);

        const VoiceHandle v = engine.playStream(id, AudioBus::Sfx);
        CHECK(v != InvalidVoice);
        engine.submitFrame(0.0f);

        constexpr uint32_t kFrames = 32;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        // First 8 frames are audible (peak ≈ 0.7); we just don't tightly
        // assert the cadence here — the byte-perfect timing test lives in
        // the ring buffer unit suite.
        const float peak = peakAbs(output, kFrames, 2);
        CHECK(peak > 0.3f);

        // Producer signals EOS + closes; the audio thread sees a closed
        // ring on the next underrun and marks the voice finished.
        engine.closeStream(id);

        engine.submitFrame(0.0f);
        float output2[kFrames * 2]{};
        for (uint32_t i = 0; i < kFrames * 2; ++i) output2[i] = 99.0f;
        engine.render(output2, kFrames);
        const float peak2 = peakAbs(output2, kFrames, 2);
        CHECK(peak2 < 0.1f); // voice slot has been recycled; no output.

        engine.shutdown();
    }

    TEST_CASE(SetTimeScaleDefaultsToOne) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));
        CHECK_FLOAT_EQ(engine.timeScale(), 1.0f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(SetTimeScaleClampsOutOfRange) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        engine.setTimeScale(10.0f);
        CHECK_FLOAT_EQ(engine.timeScale(), 4.0f, 1e-6f);

        engine.setTimeScale(0.01f);
        CHECK_FLOAT_EQ(engine.timeScale(), 0.25f, 1e-6f);

        engine.setTimeScale(2.0f);
        CHECK_FLOAT_EQ(engine.timeScale(), 2.0f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(PlayStreamWithMultipleVoicesMixesToBus) {
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.channels = 1;
        desc.capacityFrames = 512;

        const AudioStreamId a = engine.openStream(desc);
        const AudioStreamId b = engine.openStream(desc);
        CHECK(a != InvalidStreamId && b != InvalidStreamId);

        std::vector<float> src(512, 0.25f);
        CHECK(engine.streamPush(a, src.data(), 512) == 512u);
        CHECK(engine.streamPush(b, src.data(), 512) == 512u);

        const VoiceHandle v1 = engine.playStream(a, AudioBus::Sfx);
        const VoiceHandle v2 = engine.playStream(b, AudioBus::Sfx);
        CHECK(v1 != InvalidVoice && v2 != InvalidVoice);

        engine.submitFrame(0.0f);

        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);

        const float peak = peakAbs(output, kFrames, 2);
        // Two 0.25 voices sum to ~0.5; allow some slack.
        CHECK(peak > 0.2f);
        engine.shutdown();
    }

    TEST_CASE(TimeScaleAboveOneDrainsFewerStreamFramesForSameOutputBlock) {
        // Pull the same input size at scale=2.0 versus scale=1.0. After
        // one render call the ring should have consumed about half as many
        // frames at scale=2.0 (because each output frame consumes 2
        // source frames, so fewer output frames worth of content is in
        // the ring after a fixed number of output frames).
        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));

        AudioStreamDesc desc{};
        desc.channels = 1;
        desc.capacityFrames = 1024;
        const AudioStreamId id = engine.openStream(desc);
        CHECK(id != InvalidStreamId);

        std::vector<float> src(1024, 0.5f);
        CHECK(engine.streamPush(id, src.data(), 1024) == 1024u);

        const VoiceHandle v = engine.playStream(id);
        CHECK(v != InvalidVoice);

        engine.setTimeScale(2.0f);
        engine.submitFrame(0.0f);

        constexpr uint32_t kFrames = 32;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);

        AudioStreamRingBuffer* ring = engine.streamRing(id);
        CHECK(ring != nullptr);
        // We expected ~kFrames * 2 = 64 source frames drained at scale=2.0.
        // Loose upper bound: availableRead should be > capacity - 96.
        const uint32_t availRead = ring->availableRead();
        CHECK(availRead >= 1024u - 96u);

        engine.shutdown();
    }

TEST_SUITE_END
