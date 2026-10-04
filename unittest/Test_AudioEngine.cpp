#include <AYAudio.h>
#include <AYAudio/AudioBackendTestAccess.h>
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

    TEST_CASE(ClipVoiceCanSeekForEditorTimelineSync) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(48000);
        const AudioClipId clip = engine.registerClip(desc);
        const VoiceHandle voice = engine.play(clip, AudioBus::Music);
        CHECK(engine.setVoicePositionFrames(voice, 24000u));
        CHECK(engine.voicePositionFrames(voice) == 24000u);
        CHECK(engine.setVoicePositionFrames(voice, 96000u));
        CHECK(engine.voicePositionFrames(voice) == 47999u);
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

    // ===== A1 regression: voiceId identity survives same-frame allocations =====

    TEST_CASE(SameFrameTwoVoicesKeepIndependentIdentity) {
        // Audit A1: previously voice identity was derived from startFrame, so
        // two voices played in the same engine frame shared identity and the
        // finished-write-back loop would clobber the survivor. With the
        // voiceId fix each play() mints a fresh monotonic id and the render
        // snapshot maps back via that id alone.
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(64);
        const AudioClipId clip = engine.registerClip(desc);

        const VoiceHandle a = engine.play(clip, AudioBus::Sfx);
        const VoiceHandle b = engine.play(clip, AudioBus::Ui);
        CHECK(a != InvalidVoice);
        CHECK(b != InvalidVoice);
        CHECK(a != b);
        CHECK(engine.activeVoiceCount() == 2);

        // Distinct voiceIds stored on the two slots.
        const VoiceRuntime* ra = engine.voiceForHandle(a);
        const VoiceRuntime* rb = engine.voiceForHandle(b);
        CHECK(ra != nullptr);
        CHECK(rb != nullptr);
        CHECK(ra->voiceId != 0);
        CHECK(rb->voiceId != 0);
        CHECK(ra->voiceId != rb->voiceId);

        // Stop the first; the second must still be alive after render.
        engine.stop(a);
        engine.submitFrame(0.0f);
        constexpr uint32_t kFrames = 16;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        CHECK(engine.activeVoiceCount() == 1);
        CHECK(!engine.voiceForHandle(a)->playing);
        CHECK(engine.voiceForHandle(b)->playing);

        // Render again — b's read head must continue advancing, not be reset
        // by the snapshot re-binding loop.
        const uint64_t posBefore = engine.voiceForHandle(b)->positionFrames;
        engine.render(output, kFrames);
        CHECK(engine.voiceForHandle(b)->positionFrames > posBefore);

        engine.shutdown();
    }

    // ===== B1 regression: audio-thread _applyCommand finds the slot the
    // game thread populated and does not re-allocate. =====

    TEST_CASE(AudioThreadApplyCommandSkipsDualAllocate) {
        AudioEngine engine;
        engine.initialize(defaultSettings());
        AudioClipDesc desc = makeSineClip(64);
        const AudioClipId clip = engine.registerClip(desc);

        // Pre-condition: clip registry has one clip.
        CHECK(engine.clipRegistry().size() == 1u);

        // play() should populate the slot synchronously and the audio-thread
        // replay (via submitFrame + render) should observe the same voice
        // count — no second slot allocated.
        const VoiceHandle v = engine.play(clip, AudioBus::Sfx);
        CHECK(v != InvalidVoice);
        CHECK(engine.activeVoiceCount() == 1u);

        engine.submitFrame(0.0f);
        constexpr uint32_t kFrames = 8;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        // After drain the count should still be 1 (one playing voice, no
        // duplicates created by audio-thread replay).
        CHECK(engine.activeVoiceCount() == 1u);

        engine.shutdown();
    }

    // ===== A2 regression: pitch-up actually advances the read head by
    // more than 1 source frame per output frame (i.e. the stepping loop
    // ran wholeSteps > 1 iterations). The exact output sample values depend
    // on the interpolation algorithm (linear vs nearest), but the read-head
    // delta after a block must scale with pitch. =====

    TEST_CASE(PitchUpAdvancesReadHeadProportionally) {
        AudioEngine engine;
        engine.initialize(defaultSettings());

        // 1024 frames of constant 0.5 — gives plenty of headroom.
        AudioClipDesc desc = makeSineClip(1024, 0.5f);
        const AudioClipId clip = engine.registerClip(desc);
        const VoiceHandle v = engine.play(clip, AudioBus::Sfx,
                                          SpatializationParams{},
                                          /*volume=*/1.0f,
                                          /*loop=*/true);
        CHECK(v != InvalidVoice);
        engine.submitFrame(0.0f);

        // Pitch 1.0 baseline.
        engine.setVoicePitch(v, 1.0f);
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        engine.render(output, kFrames);
        const uint64_t posAtPitch1 = engine.voiceForHandle(v)->positionFrames;
        CHECK(posAtPitch1 > 0u);

        // Now stop, restart with pitch 2.0, render again, and confirm the
        // read head advances roughly twice as far for the same output size.
        engine.stop(v);
        engine.submitFrame(0.0f);
        const VoiceHandle v2 = engine.play(clip, AudioBus::Sfx,
                                           SpatializationParams{},
                                           /*volume=*/1.0f,
                                           /*loop=*/true);
        engine.setVoicePitch(v2, 2.0f);
        engine.submitFrame(0.0f);
        engine.render(output, kFrames);
        const uint64_t posAtPitch2 = engine.voiceForHandle(v2)->positionFrames;
        // Pitch 2 should advance at least ~1.5× pitch 1 over a 64-frame
        // block (allow wide slack for the fractional accumulator settling
        // across the very first call).
        CHECK(posAtPitch2 > posAtPitch1 + (posAtPitch1 / 2));
        engine.shutdown();
    }

    TEST_CASE(OutputPeaksSeparateChannelsAndReportPreClampOverload) {
        AudioEngine engine;auto settings=defaultSettings();settings.reverbReturn=0;
        CHECK(engine.initialize(settings));
        std::vector<float> pcm(128,0);for(size_t i=0;i<pcm.size();i+=2)pcm[i]=0.9f;
        AudioClipDesc desc;desc.channels=2;desc.frameCount=64;desc.pcmData=pcm.data();
        const auto voice=engine.play(engine.registerClip(desc),AudioBus::Sfx,{},2,true);
        float out[32]{};engine.render(out,16);auto levels=engine.outputLevels();
        CHECK(levels.left>1);CHECK(levels.right==0);CHECK(levels.clipped);CHECK(out[0]==1);
        const auto block=levels.block;engine.pause();engine.render(out,16);levels=engine.outputLevels();
        CHECK(levels.block>block);CHECK(levels.left==0);CHECK(levels.right==0);CHECK(!levels.clipped);
        engine.resume();engine.setMasterGain(0);engine.render(out,16);CHECK(engine.outputLevels().left==0);
        engine.shutdown();CHECK(engine.outputLevels().left==0);
    }
    TEST_CASE(MonoOutputTelemetryMirrorsBothChannels) {
        AudioEngine engine;auto settings=defaultSettings();settings.channels=1;settings.reverbReturn=0;
        CHECK(engine.initialize(settings));engine.play(engine.registerClip(makeSineClip(64,.3f)));
        float out[8]{};engine.render(out,8);const auto levels=engine.outputLevels();
        CHECK(levels.left>0);CHECK(levels.left==levels.right);CHECK(!levels.clipped);
    }

    TEST_CASE(LargeOutputBlockRetainsPeakFromFirstChunk) {
        AudioEngine engine;auto settings=defaultSettings();settings.reverbReturn=0;CHECK(engine.initialize(settings));
        std::vector<float> pcm(5000,0);pcm[0]=.8f;
        AudioClipDesc desc;desc.channels=1;desc.frameCount=5000;desc.pcmData=pcm.data();
        engine.play(engine.registerClip(desc));std::vector<float> output(10000);engine.render(output.data(),5000);
        CHECK(engine.outputLevels().left>0);CHECK(engine.outputLevels().left==engine.outputLevels().right);
    }
TEST_SUITE_END
