#pragma once
// MiniaudioBackend.h - Real device backend via miniaudio (internal header)

#include <IAudioBackend.h>
#include <AYAudioEngine.h>

struct ma_device;
struct ma_context;

namespace ayt::audio
{

class MiniaudioBackend : public IAudioBackend {
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    void attachEngine(AudioEngine* engine);
    void bindAudioEngine(AudioEngine* engine) override { attachEngine(engine); }

    // Invoked from the miniaudio data callback (audio thread).
    void pumpFromEngine(float* output, unsigned int frameCount);

    bool start(const AudioBackendInfo& info) override;
    void stop() override;
    void render(float* output, uint32_t frameCount, uint32_t channels) override;
    AudioBackendInfo describe() const override;
    bool isRealDevice() const override;

private:
    AudioEngine* _engine = nullptr;
    ma_context* _context = nullptr;
    ma_device* _device = nullptr;
    AudioBackendInfo _info{};
    bool _started = false;
};

std::unique_ptr<IAudioBackend> makeMiniaudioBackend();

} // namespace ayt::audio
