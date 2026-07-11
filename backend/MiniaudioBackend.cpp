// MiniaudioBackend.cpp - Real device backend via miniaudio (v0.5+)
//
// v0 ships the interface and the Null/Mock backends. The MiniaudioBackend
// implementation lands in v0.5 once we vendor `miniaudio.h` under
// `thirdparty/`. Until then this TU provides a stub so the build doesn't
// break when the source file is referenced by the CMake target.
//
// To enable: drop `miniaudio.h` into AYAudio/thirdparty/ and uncomment the
// implementation below. The stub compiles to an object that links cleanly
// but `start()` returns false (no device).

#include <IAudioBackend.h>

#include <cstring>
#include <memory>

namespace ayt::audio
{

namespace {

// Stub used while miniaudio is not vendored. Marks itself as a real-device
// backend so callers can detect "would have used hardware" vs. mock/null.
class MiniaudioBackendStub final : public IAudioBackend {
public:
    bool start(const AudioBackendInfo& info) override {
        _info = info;
        _info.realDevice = true;
        // No miniaudio in tree yet — refuse to start so callers fall back.
        return false;
    }

    void stop() override {
        _info = AudioBackendInfo{};
    }

    void render(float* output, uint32_t frameCount, uint32_t channels) override {
        if (output != nullptr) {
            std::memset(output, 0, frameCount * channels * sizeof(float));
        }
    }

    AudioBackendInfo describe() const override { return _info; }
    bool isRealDevice() const override { return false; }

private:
    AudioBackendInfo _info{};
};

} // namespace

std::unique_ptr<IAudioBackend> makeMiniaudioBackend() {
    return std::unique_ptr<IAudioBackend>(new MiniaudioBackendStub());
}

} // namespace ayt::audio