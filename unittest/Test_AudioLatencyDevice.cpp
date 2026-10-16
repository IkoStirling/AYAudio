#include <AYAudio/AudioEngine.h>
#include <AYAudio/AudioRecorder.h>
#include <AYAudio/MidiInput.h>
#include "MiniaudioBackend.h"
#include <AYTest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
using namespace ayt::audio;
TEST_SUITE(AudioLatencyDeviceTests)
TEST_CASE(WasapiProbeProducesMeasuredRoundTrip) {
#ifndef _WIN32
    SKIP_TEST("WASAPI round-trip measurement is Windows-only");
#else
    if (!std::getenv("AY_AUDIO_RECORDING_DEVICE_TEST"))
        SKIP_TEST("Opt-in: AY_AUDIO_RECORDING_DEVICE_TEST=1");
    // Warm the actual engine output before capture: device initialization is not latency.
    AudioEngine engine;
    engine.setBackend(makeMiniaudioBackend());
    AudioSettings settings;
    settings.sampleRate = 48000;
    settings.channels = 2;
    CHECK(engine.initialize(settings));
    AudioRecorder capture;
    std::vector<RecordingDevice> devices;
    std::string error;
    CHECK(capture.enumerateDevices(devices, &error));
    RecordingConfig config;
    config.loopback = true;
    config.maxSeconds = 2;
    for (const auto& device : devices)
        if (!device.input && device.isDefault) config.outputDevice = device.id;
    CHECK(capture.start(config, &error));
    std::vector<float> probe(256);
    uint32_t random = 0x9e3779b9;
    for (auto& sample : probe) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        sample = float(int(random % 2001) - 1000) * .0002f;
    }
    AudioClipDesc clip;
    clip.channels = 1;
    clip.sampleRate = 48000;
    clip.frameCount = uint32_t(probe.size());
    clip.pcmData = probe.data();
    const auto id = engine.registerClip(clip);
    CHECK(id != 0);
    capture.update();
    const auto origin = capture.status().frames;
    CHECK(engine.play(id, AudioBus::Preview) != 0);
    for (int i = 0; i < 80; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        capture.update();
    }
    capture.stop();
    const auto status = capture.status();
    const auto& captured = capture.samples();
    std::vector<float> mono(size_t(status.frames - origin));
    for (size_t f = 0; f < mono.size(); ++f)
        for (size_t c = 0; c < status.channels; ++c)
            mono[f] += captured[(f + origin) * status.channels + c] / status.channels;
    AudioLatencyEstimate estimate;
    const auto maxLag = uint32_t(std::min<size_t>(24000, mono.size() > probe.size() ? mono.size() - probe.size() : 0));
    CHECK_MSG(estimateAudioLatency(probe, mono, 48000, 0, maxLag, estimate, &error), error.c_str());
    CHECK(estimate.confidence >= .6);
    CHECK(status.droppedFrames == 0);
    std::fprintf(stderr, "[AudioLatency] roundTripMs=%.3f correlation=%.6f frames=%llu dropped=%llu\n", estimate.seconds * 1000, estimate.confidence, static_cast<unsigned long long>(status.frames), static_cast<unsigned long long>(status.droppedFrames));
#endif
}
TEST_SUITE_END