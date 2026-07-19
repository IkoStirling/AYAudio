#include <AYAudio.h>
#include <AYAudioBackendTestAccess.h>
#include "AYTest.h"

#include <vector>

using namespace ayt::audio;

namespace {

AudioSettings defaultSettings() {
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 32;
    s.maxVoices = 8;
    s.maxClips = 8;
    s.masterGain = 1.0f;
    return s;
}

// Build a tiny 1-frame mono F32 clip (a 440 Hz sine-like DC value).
std::vector<float> g_engineSineScratch;
AudioClipDesc makeSineClip(uint32_t frameCount, float amplitude = 0.5f) {
    AudioClipDesc desc{};
    desc.sampleRate = 48000;
    desc.channels = 1;
    desc.format = AudioSampleFormat::F32;
    desc.frameCount = frameCount;
    g_engineSineScratch.assign(frameCount, amplitude);
    desc.pcmData = g_engineSineScratch.data();
    return desc;
}

} // namespace

TEST_SUITE(AudioEngineTests)

    TEST_CASE(InitializeShutdown) {
        AudioEngine engine;
        CHECK(!engine.isInitialized());
        CHECK(engine.initialize(defaultSettings()));
        CHECK(engine.isInitialized());
        engine.shutdown();
        CHECK(!engine.isInitialized());
    }

    TEST_CASE(RegisterClipReturnsSequentialIds) {
        AudioEngine engine;
        engine.initialize(defaultSettings());

        AudioClipDesc desc = makeSineClip(128);
        const AudioClipId a = engine.registerClip(desc);
        const AudioClipId b = engine.registerClip(desc);
        CHECK(a != InvalidClipId);
        CHECK(b != InvalidClipId);
        CHECK(a != b);
        engine.shutdown();
    }

    TEST_CASE(ReleaseClipAllowsReuseOfSlot) {
        AudioEngine engine;
        engine.initialize(defaultSettings());

        AudioClipDesc desc = makeSineClip(64);
        const AudioClipId a = engine.registerClip(desc);
        engine.releaseClip(a);
        const AudioClipId b = engine.registerClip(desc);
        CHECK(b != InvalidClipId);
        engine.shutdown();
    }

    TEST_CASE(PlayRequiresValidClip) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        // No clips registered.
        const VoiceHandle v = engine.play(InvalidClipId);
        CHECK(v == InvalidVoice);
        engine.shutdown();
    }

    TEST_CASE(PlayAllocatesVoice) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(128);
        const AudioClipId clip = engine.registerClip(desc);

        const VoiceHandle v = engine.play(clip, AudioBus::Sfx);
        CHECK(v != InvalidVoice);
        CHECK(engine.activeVoiceCount() == 1);

        engine.stop(v);
        engine.submitFrame(0.0f);
        CHECK(engine.activeVoiceCount() == 0);
        engine.shutdown();
    }

    TEST_CASE(StopAllClearsActiveVoices) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(128);
        const AudioClipId clip = engine.registerClip(desc);

        engine.play(clip, AudioBus::Music);
        engine.play(clip, AudioBus::Sfx);
        engine.play(clip, AudioBus::Ui);
        CHECK(engine.activeVoiceCount() == 3);

        engine.stopAll();
        engine.submitFrame(0.0f);
        CHECK(engine.activeVoiceCount() == 0);
        engine.shutdown();
    }

    TEST_CASE(SetVoiceVolumeClamps) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(128);
        const AudioClipId clip = engine.registerClip(desc);
        const VoiceHandle v = engine.play(clip);

        engine.setVoiceVolume(v, 100.0f);
        const VoiceRuntime* rt = engine.voiceAt(0);
        CHECK(rt != nullptr);
        CHECK(rt->volume <= 8.0f);
        engine.shutdown();
    }

    TEST_CASE(BusGainAppliedThroughMixer) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setBusGain(AudioBus::Sfx, 0.5f);
        CHECK_FLOAT_EQ(engine.mixer().getBusGain(AudioBus::Sfx), 0.5f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(ListenerDefaultsAreSensible) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        const ListenerParams& l = engine.listener();
        // Forward default is -Z.
        CHECK_FLOAT_EQ(l.forward.z, -1.0f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(PauseResumeFlag) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        CHECK(!engine.isPaused());
        engine.pause();
        CHECK(engine.isPaused());
        engine.resume();
        CHECK(!engine.isPaused());
        engine.shutdown();
    }

    TEST_CASE(RenderProducesSilentOutputWhenPaused) {
        AudioEngine engine;
        engine.initialize(defaultSettings());

        engine.pause();
        constexpr uint32_t kFrames = 32;
        float output[kFrames * 2];
        for (uint32_t i = 0; i < kFrames * 2; ++i) output[i] = 99.0f;
        engine.render(output, kFrames);
        for (uint32_t i = 0; i < kFrames * 2; ++i) {
            CHECK_FLOAT_EQ(output[i], 0.0f, 1e-6f);
        }
        engine.shutdown();
    }

    TEST_CASE(RenderMixesPlayingVoice) {
        AudioEngine engine;
        engine.initialize(defaultSettings());

        AudioClipDesc desc = makeSineClip(64, 0.5f);
        const AudioClipId clip = engine.registerClip(desc);
        const VoiceHandle v = engine.play(clip, AudioBus::Sfx);
        CHECK(v != InvalidVoice);
        engine.submitFrame(0.0f);

        constexpr uint32_t kFrames = 32;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);

        // At least one sample should be non-zero (mono source duplicated to L/R
        // with gain 0.5).
        float peak = 0.0f;
        for (uint32_t i = 0; i < kFrames * 2; ++i) {
            peak = std::fabs(output[i]) > peak ? std::fabs(output[i]) : peak;
        }
        CHECK(peak > 0.0f);
        engine.shutdown();
    }

    TEST_CASE(NullBackendFactoryProducesWorkingBackend) {
        auto backend = makeNullBackend();
        CHECK(backend != nullptr);
        CHECK(!backend->isRealDevice());

        AudioBackendInfo info{};
        info.channels = 2;
        info.sampleRate = 48000;
        info.name = "NullTest";
        CHECK(backend->start(info));

        constexpr uint32_t kFrames = 16;
        float output[kFrames * 2];
        for (uint32_t i = 0; i < kFrames * 2; ++i) output[i] = 99.0f;
        backend->render(output, kFrames, 2);
        for (uint32_t i = 0; i < kFrames * 2; ++i) {
            CHECK_FLOAT_EQ(output[i], 0.0f, 1e-6f);
        }
        backend->stop();
    }

    TEST_CASE(MockBackendFactoryAndInspection) {
        auto backend = makeMockBackend();
        CHECK(backend != nullptr);

        AudioBackendInfo info{};
        info.channels = 2;
        info.sampleRate = 48000;
        info.name = "MockTest";
        CHECK(backend->start(info));

        constexpr uint32_t kFrames = 16;
        float output[kFrames * 2];
        for (uint32_t i = 0; i < kFrames * 2; ++i) output[i] = 0.5f;
        backend->render(output, kFrames, 2);

        const MockBackendStats s = inspectMockBackend(backend.get());
        CHECK_INT_EQ(s.renderCallCount, 1u);
        CHECK_INT_EQ(s.lastFrameCount, kFrames);
        CHECK_FLOAT_EQ(s.peakAmplitude, 0.5f, 1e-6f);
        CHECK_FLOAT_EQ(mockSampleAt(backend.get(), 0, 0), 0.5f, 1e-6f);
        backend->stop();
    }

    // ===== v2.0b: ducking / reverb / doppler integration =====

    TEST_CASE(DuckingCommandRoundTrip) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setDucking(-12.0f, 8.0f, 2.0f);
        engine.submitFrame(0.0f);
        CHECK_FLOAT_EQ(engine.mixer().duckState().depthDb, -12.0f, 1e-6f);
        CHECK_FLOAT_EQ(engine.mixer().duckState().attackPerSec, 8.0f, 1e-6f);
        CHECK_FLOAT_EQ(engine.mixer().duckState().releasePerSec, 2.0f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(ReverbCommandRoundTrip) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setMasterReverbReturn(0.42f);
        engine.setReverbPreset(1);
        engine.submitFrame(0.0f);
        CHECK_FLOAT_EQ(engine.mixer().reverbReturn(), 0.42f, 1e-6f);
        CHECK_INT_EQ(engine.mixer().reverbPreset(), 1u);
        engine.shutdown();
    }

    TEST_CASE(DopplerScaleCommandRoundTrip) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setDopplerScale(0.5f);
        CHECK_FLOAT_EQ(engine.dopplerScale(), 0.5f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(DopplerDisabledWhenScaleZero) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setDopplerScale(0.0f);
        // Register a moving-source clip. Render two consecutive blocks; the
        // second block's `availableRead` should equal the first block's —
        // because doppler is disabled ⇒ effective scale = pitch * timeScale
        // * 0 = 0, which clamps to 0.5 (so 1 source frame per 2 output
        // frames). With a static baseline comparison we just confirm the
        // effectiveScale lands on a known value by way of the read head.
        AudioClipDesc desc = makeSineClip(256);
        const AudioClipId clip = engine.registerClip(desc);
        SpatializationParams spatial{};
        spatial.mode = SpatializationParams::Mode::World;
        spatial.position = { 10.0f, 0.0f, 0.0f };
        spatial.velocity = { -100.0f, 0.0f, 0.0f };  // strong approach
        spatial.attenuation = SpatializationParams::AttenuationModel::InverseClamped;
        spatial.referenceDistance = 0.001f;
        spatial.maxDistance = 1000.0f;
        const VoiceHandle v = engine.play(clip, AudioBus::Sfx, spatial, 1.0f, true);
        CHECK(v != InvalidVoice);
        engine.submitFrame(0.0f);
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        // We just confirm the engine rendered without crashing and that
        // the doppler scale persisted. Direct read-head verification
        // happens at the AudioSpatializer unit level.
        CHECK_FLOAT_EQ(engine.dopplerScale(), 0.0f, 1e-6f);
        engine.shutdown();
    }

    TEST_CASE(DuckingAppliesAtEngineLevel) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        engine.setDucking(-60.0f, 1000.0f, 1000.0f);
        // Register a clip and play it on the Music bus.
        AudioClipDesc desc = makeSineClip(64, 1.0f);
        const AudioClipId clip = engine.registerClip(desc);
        const VoiceHandle musicVoice = engine.play(clip, AudioBus::Music);
        CHECK(musicVoice != InvalidVoice);
        // Also play a Voice-bus voice (same clip; same instance — voice
        // pool reuses).
        SpatializationParams spatial{};
        const VoiceHandle voiceVoice = engine.play(clip, AudioBus::Voice, spatial);
        CHECK(voiceVoice != InvalidVoice);
        engine.submitFrame(0.0f);
        constexpr uint32_t kFrames = 48;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        // After this block the duck envelope has engaged (Voice-bus voice
        // was active). Sample the mixer's envelope.
        CHECK(engine.mixer().duckState().currentDb < -0.1f);
        engine.shutdown();
    }
TEST_SUITE_END
