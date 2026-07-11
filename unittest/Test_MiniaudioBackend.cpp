#include <AYAudio.h>
#include "MiniaudioBackend.h"

#include "AYTest.h"

using namespace ayt::audio;

namespace {

// Helper: try to open the default playback device via MiniaudioBackend. If
// no device is available (CI, headless, no sound card), the backend refuses
// to start and the test that called this should bail out without asserting.
bool tryOpenBackend(MiniaudioBackend& backend, AudioBackendInfo& infoOut) {
    AudioBackendInfo info{};
    info.name = "TestDevice";
    info.sampleRate = 48000;
    info.channels = 2;
    info.bitsPerSample = 32;
    info.realDevice = true;
    if (!backend.start(info)) {
        return false;
    }
    infoOut = backend.describe();
    return true;
}

} // namespace

TEST_SUITE(MiniaudioBackendTests)

    // Smoke: the backend struct instantiates and reports "not a real device"
    // before start(). This always passes regardless of hardware.
    TEST_CASE(FreshBackendIsNotRealDevice) {
        MiniaudioBackend backend;
        CHECK(!backend.isRealDevice());
        const AudioBackendInfo info = backend.describe();
        CHECK(info.name == nullptr);
        CHECK(info.sampleRate == 0);
        CHECK(info.channels == 0);
        CHECK(!info.realDevice);
    }

    // Real-device lifecycle. If no audio device is reachable from this
    // process (headless / no sound card), we skip rather than fail so the
    // test suite still passes in CI.
    TEST_CASE(StartStopRoundTrip) {
        MiniaudioBackend backend;
        AudioBackendInfo opened{};
        if (!tryOpenBackend(backend, opened)) {
            ::printf("  [SKIP] no audio device available\n");
            return;
        }
        CHECK(backend.isRealDevice());
        // After start() the backend should advertise the channels it actually
        // opened (post-remap); sample rate is whatever miniaudio picked.
        CHECK(opened.channels == 2 || opened.channels == 1);
        CHECK(opened.sampleRate > 0);

        backend.stop();
        CHECK(!backend.isRealDevice());
        const AudioBackendInfo after = backend.describe();
        CHECK(after.name == nullptr);
    }

    // Double-start should be idempotent.
    TEST_CASE(DoubleStartIsIdempotent) {
        MiniaudioBackend backend;
        AudioBackendInfo opened{};
        if (!tryOpenBackend(backend, opened)) {
            ::printf("  [SKIP] no audio device available\n");
            return;
        }
        const AudioBackendInfo first = backend.describe();
        // Second start must succeed without touching device state.
        CHECK(backend.start(first));
        CHECK(backend.isRealDevice());
        backend.stop();
    }

    // AudioEngine wires itself onto the backend on initialize(). We exercise
    // that path end-to-end: open device, attach engine, play a tiny clip,
    // submit one frame, then tear down. Skips when no device is available.
    TEST_CASE(AudioEnginePlaysThroughMiniaudioBackend) {
        AudioEngine engine;
        auto backend = std::unique_ptr<IAudioBackend>(new MiniaudioBackend());
        engine.setBackend(std::move(backend));

        AudioSettings settings{};
        settings.backend = AudioBackendKind::Miniaudio;
        settings.sampleRate = 48000;
        settings.channels = 2;
        settings.commandQueueCapacity = 32;
        settings.maxVoices = 4;
        settings.maxClips = 4;
        settings.masterGain = 1.0f;

        if (!engine.initialize(settings)) {
            ::printf("  [SKIP] no audio device available\n");
            engine.shutdown();
            return;
        }
        CHECK(engine.isInitialized());

        // Register a 1-frame mono F32 clip and play it once.
        const float pcm[1] = { 0.25f };
        AudioClipDesc desc{};
        desc.sampleRate = 48000;
        desc.channels = 1;
        desc.format = AudioSampleFormat::F32;
        desc.frameCount = 1;
        desc.pcmData = pcm;
        const AudioClipId clip = engine.registerClip(desc);
        CHECK(clip != InvalidClipId);

        const VoiceHandle voice = engine.play(clip, AudioBus::Sfx);
        CHECK(voice != InvalidVoice);
        CHECK(engine.activeVoiceCount() == 1);

        // Submit one game frame so the engine sees the queued Play command.
        engine.submitFrame(0.016f);

        // Drive a few frames of render manually — the real device callback
        // would do this, but tests want determinism.
        float scratch[64 * 2] = {};
        engine.render(scratch, 64);

        engine.stopAll();
        engine.shutdown();
        CHECK(!engine.isInitialized());
    }

TEST_SUITE_END