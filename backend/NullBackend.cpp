// NullBackend.cpp - Audio backend that discards all output
//
// Selected when AYApplication sets noAudio=true (CLI -no-audio) or when a
// build wants audio playback disabled. The engine still flows commands,
// allocates voices, and runs the mixer — output just goes nowhere.
//
// Useful as a default for headless servers, automated tests that don't need
// the mixer exercised, and the Editor's "audio off" mode.

#include <IAudioBackend.h>
#include <cstring>
#include <memory>

namespace ayt::audio
{

class NullBackend final : public IAudioBackend {
public:
    bool start(const AudioBackendInfo& info) override {
        _info = info;
        if (_info.name == nullptr) {
            _info.name = "Null";
        }
        return true;
    }

    void stop() override {
        _info = AudioBackendInfo{};
    }

    void render(float* output, uint32_t frameCount, uint32_t channels) override {
        if (output != nullptr && frameCount > 0 && channels > 0) {
            std::memset(output, 0, frameCount * channels * sizeof(float));
        }
    }

    AudioBackendInfo describe() const override { return _info; }
    bool isRealDevice() const override { return false; }

private:
    AudioBackendInfo _info{};
};

std::unique_ptr<IAudioBackend> makeNullBackend() {
    return std::unique_ptr<IAudioBackend>(new NullBackend());
}

} // namespace ayt::audio