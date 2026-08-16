#include <AYAudio.h>
#include "MiniaudioBackend.h"
#include "AYTest.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace ayt::audio;

namespace {

AudioBackendInfo defaultDeviceInfo() {
    AudioBackendInfo info{};
    info.sampleRate = 48000;
    info.channels = 2;
    info.bitsPerSample = 32;
    return info;
}

} // namespace

TEST_SUITE(MiniaudioBackendTests)

    TEST_CASE(FreshBackendIsNotRealDevice) {
        auto backend = makeMiniaudioBackend();
        CHECK(backend != nullptr);
        CHECK(!backend->isRealDevice());
    }

    TEST_CASE(StartStopRoundTrip) {
        auto backend = makeMiniaudioBackend();
        CHECK(backend != nullptr);
        if (!backend->start(defaultDeviceInfo())) {
            ::printf("  [SKIP] StartStopRoundTrip: no playback device\n");
            return;
        }
        CHECK(backend->isRealDevice());
        backend->stop();
        CHECK(!backend->isRealDevice());
    }

    TEST_CASE(DoubleStartIsIdempotent) {
        auto backend = makeMiniaudioBackend();
        CHECK(backend != nullptr);
        if (!backend->start(defaultDeviceInfo())) {
            ::printf("  [SKIP] DoubleStartIsIdempotent: no playback device\n");
            return;
        }
        CHECK(backend->start(defaultDeviceInfo()));
        backend->stop();
    }

    TEST_CASE(AudioEnginePlaysThroughMiniaudioBackend) {
        AudioSettings settings{};
        settings.backend = AudioBackendKind::Miniaudio;
        settings.sampleRate = 48000;
        settings.channels = 2;
        settings.commandQueueCapacity = 32;
        settings.maxVoices = 4;
        settings.maxClips = 4;
        settings.masterGain = 1.0f;

        AudioEngine engine;
        engine.setBackend(makeMiniaudioBackend());
        if (!engine.initialize(settings)) {
            ::printf("  [SKIP] AudioEnginePlaysThroughMiniaudioBackend: "
                     "device open failed\n");
            return;
        }
        CHECK(engine.isInitialized());
        CHECK(engine.backend() != nullptr);
        CHECK(engine.backend()->isRealDevice());
        // Let the audio callback pump a few times before teardown.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        engine.shutdown();
        CHECK(!engine.isInitialized());
    }

TEST_SUITE_END
