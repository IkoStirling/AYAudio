// MockBackend.cpp - Test backend that captures rendered frames
//
// Records the most recent render() call into an internal buffer. Tests can
// inspect peak amplitude, frame count, and a sample of the rendered signal
// to validate mixing / spatialization behavior without a sound card.
//
// This backend is NOT threadsafe — tests drive it from a single thread.

#include <IAudioBackend.h>
#include <AYAudioBackendTestAccess.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace ayt::audio
{

class MockBackend final : public IAudioBackend {
public:
    bool start(const AudioBackendInfo& info) override {
        _info = info;
        if (_info.name == nullptr) {
            _info.name = "Mock";
        }
        _peak = 0.0f;
        _lastBuffer.assign(static_cast<size_t>(info.channels) * 4096, 0.0f);
        _lastFrameCount = 0;
        _renderCallCount = 0;
        return true;
    }

    void stop() override {
        _info = AudioBackendInfo{};
        _lastBuffer.clear();
        _lastFrameCount = 0;
        _peak = 0.0f;
    }

    void render(float* output, uint32_t frameCount, uint32_t channels) override {
        if (output == nullptr || frameCount == 0 || channels == 0) {
            return;
        }
        const size_t totalSamples = static_cast<size_t>(frameCount) * channels;
        if (_lastBuffer.size() < totalSamples) {
            _lastBuffer.resize(totalSamples, 0.0f);
        }
        std::memcpy(_lastBuffer.data(), output, totalSamples * sizeof(float));

        float peak = 0.0f;
        for (size_t i = 0; i < totalSamples; ++i) {
            const float v = std::fabs(output[i]);
            if (v > peak) peak = v;
        }
        _peak = peak;
        _lastFrameCount = frameCount;
        ++_renderCallCount;
    }

    AudioBackendInfo describe() const override { return _info; }
    bool isRealDevice() const override { return false; }

    // ===== Inspection helpers (test-only) =====

    uint32_t renderCallCount() const { return _renderCallCount; }
    uint32_t lastFrameCount() const { return _lastFrameCount; }
    float peakAmplitude() const { return _peak; }

    // Read a sample from the most recent render buffer. Returns 0 when no
    // render has happened yet or the index is out of range.
    float sampleAt(uint32_t frame, uint32_t channel) const {
        if (_lastBuffer.empty()) return 0.0f;
        const uint32_t channels = _info.channels ? _info.channels : 1;
        const size_t idx = static_cast<size_t>(frame) * channels + channel;
        if (idx >= _lastBuffer.size()) return 0.0f;
        return _lastBuffer[idx];
    }

private:
    AudioBackendInfo _info{};
    std::vector<float> _lastBuffer;     // most recent rendered frame block
    uint32_t _lastFrameCount = 0;
    uint32_t _renderCallCount = 0;
    float _peak = 0.0f;
};

std::unique_ptr<IAudioBackend> makeMockBackend() {
    return std::unique_ptr<IAudioBackend>(new MockBackend());
}

MockBackendStats inspectMockBackend(IAudioBackend* backend) {
    if (auto* m = dynamic_cast<MockBackend*>(backend)) {
        MockBackendStats s{};
        s.renderCallCount = m->renderCallCount();
        s.lastFrameCount  = m->lastFrameCount();
        s.peakAmplitude   = m->peakAmplitude();
        return s;
    }
    return MockBackendStats{};
}

float mockSampleAt(IAudioBackend* backend, uint32_t frame, uint32_t channel) {
    if (auto* m = dynamic_cast<MockBackend*>(backend)) {
        return m->sampleAt(frame, channel);
    }
    return 0.0f;
}

} // namespace ayt::audio