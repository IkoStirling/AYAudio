#pragma once
// MiniaudioBackend.h - Real device backend via miniaudio (internal header)
//
// This file is private to the backend TU and AudioEngine.cpp. It exists so
// the engine can attach itself via attachEngine() once the backend is in
// place, without leaking miniaudio's types into IAudioBackend.

#include <IAudioBackend.h>
#include <AYAudioEngine.h>

// miniaudio.h forward declarations only — never include the full header here
// or we'll double-include the implementation.
struct ma_device;

namespace ayt::audio
{

class MiniaudioBackend : public IAudioBackend {
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    // Called by AudioEngine::initialize() once the backend is owned. The
    // miniaudio data callback forwards to AudioEngine::render() so the
    // engine drives output rather than pulling itself.
    void attachEngine(AudioEngine* engine);

    // Pump `frameCount` interleaved float32 frames into `output`. Invoked
    // from the audio thread by the miniaudio data callback.
    void pumpFromEngine(float* output, unsigned int frameCount);

    // IAudioBackend
    bool start(const AudioBackendInfo& info) override;
    void stop() override;
    void render(float* output, uint32_t frameCount, uint32_t channels) override;
    AudioBackendInfo describe() const override;
    bool isRealDevice() const override;

private:
    AudioEngine* _engine = nullptr;
    ma_device* _device = nullptr;        // owned, allocated in start()
    AudioBackendInfo _info{};
    bool _started = false;
};

std::unique_ptr<IAudioBackend> makeMiniaudioBackend();

} // namespace ayt::audio