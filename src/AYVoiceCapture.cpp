#include <AYAudio/VoiceCapture.h>
#include <AYAudio/AudioStreamRingBuffer.h>
#include <miniaudio.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace ayt::audio {
namespace {
bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}
bool valid(const VoiceCaptureConfig& config) {
    return (config.sampleRate == 8'000 || config.sampleRate == 16'000 ||
            config.sampleRate == 24'000 || config.sampleRate == 48'000) &&
           (config.channels == 1 || config.channels == 2);
}
}

struct VoiceCapture::Impl {
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;
    std::atomic<bool> active{false}, muted{false};
    std::atomic<std::uint64_t> dropped{0};
    VoiceCaptureConfig config{};
    AudioStreamRingBuffer ring;

    bool initialize(std::string* error) {
        if (contextReady) return true;
#if defined(_WIN32)
        const ma_backend backend = ma_backend_wasapi;
        const auto result = ma_context_init(&backend, 1, nullptr, &context);
#else
        const auto result = ma_context_init(nullptr, 0, nullptr, &context);
#endif
        if (result != MA_SUCCESS) return fail(error, "Cannot initialize voice capture backend");
        contextReady = true;
        return true;
    }
    void receive(const float* pcm, std::uint32_t frames) noexcept {
        if (!active.load(std::memory_order_acquire) ||
            muted.load(std::memory_order_acquire) || !pcm) return;
        const auto written = ring.write(pcm, frames);
        dropped.fetch_add(frames - written, std::memory_order_relaxed);
    }
    static void callback(ma_device* device, void*, const void* input, ma_uint32 frames) {
        static_cast<Impl*>(device->pUserData)->receive(
            static_cast<const float*>(input), frames);
    }
    void discard() noexcept {
        float scratch[1024];
        const auto block = static_cast<std::uint32_t>(1024 / config.channels);
        while (ring.read(scratch, block) != 0) {}
    }
};

VoiceCapture::VoiceCapture() : _impl(std::make_unique<Impl>()) {}
VoiceCapture::~VoiceCapture() {
    stop();
    if (_impl->contextReady) ma_context_uninit(&_impl->context);
}
bool VoiceCapture::enumerateDevices(std::vector<RecordingDevice>& out,
                                    std::string* error) {
    out.clear();
    auto& state = *_impl;
    if (state.active) return fail(error, "Stop voice capture before refreshing devices");
    if (!state.initialize(error)) return false;
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0, captureCount = 0;
    if (ma_context_get_devices(&state.context, &playback, &playbackCount,
                               &capture, &captureCount) != MA_SUCCESS)
        return fail(error, "Cannot enumerate voice input devices");
    for (ma_uint32 i = 0; i < captureCount; ++i)
        out.push_back({"input/" + std::to_string(i), capture[i].name, true,
                       capture[i].isDefault != 0});
    if (error) error->clear();
    return true;
}
bool VoiceCapture::start(const VoiceCaptureConfig& config, std::string* error) {
    auto& state = *_impl;
    if (state.active || state.deviceReady)
        return fail(error, "Voice capture is already active");
    if (!valid(config)) return fail(error, "Invalid voice capture format");
    if (!state.initialize(error)) return false;
    ma_device_id selectedId{};
    const ma_device_id* selected = nullptr;
    if (!config.inputDevice.empty()) {
        ma_device_info* playback = nullptr;
        ma_device_info* capture = nullptr;
        ma_uint32 playbackCount = 0, captureCount = 0;
        if (ma_context_get_devices(&state.context, &playback, &playbackCount,
                                   &capture, &captureCount) != MA_SUCCESS)
            return fail(error, "Cannot enumerate voice input devices");
        bool found = false;
        for (ma_uint32 i = 0; i < captureCount; ++i) {
            if (config.inputDevice != "input/" + std::to_string(i)) continue;
            selectedId = capture[i].id;
            selected = &selectedId;
            found = true;
            break;
        }
        if (!found) return fail(error, "Selected voice input device disappeared");
    }
    if (!state.ring.initialize(config.channels, config.sampleRate / 2))
        return fail(error, "Cannot allocate voice capture ring");
    auto native = ma_device_config_init(ma_device_type_capture);
    native.capture.pDeviceID = selected;
    native.capture.format = ma_format_f32;
    native.capture.channels = config.channels;
    native.sampleRate = config.sampleRate;
    native.dataCallback = Impl::callback;
    native.pUserData = &state;
    if (ma_device_init(&state.context, &native, &state.device) != MA_SUCCESS) {
        state.ring.shutdown();
        return fail(error, "Cannot open voice input device");
    }
    state.deviceReady = true;
    state.config = config;
    state.dropped = 0;
    state.muted = false;
    state.active = true;
    if (ma_device_start(&state.device) != MA_SUCCESS) {
        stop();
        return fail(error, "Cannot start voice input device");
    }
    if (error) error->clear();
    return true;
}
void VoiceCapture::stop() noexcept {
    auto& state = *_impl;
    state.active = false;
    if (state.deviceReady) {
        ma_device_uninit(&state.device);
        state.deviceReady = false;
    }
    state.ring.shutdown();
}
bool VoiceCapture::running() const noexcept { return _impl->active; }
void VoiceCapture::setMuted(bool muted) noexcept {
    auto& state = *_impl;
    if (muted) {
        state.muted = true;
        state.discard();
    } else {
        state.discard();
        state.muted = false;
    }
}
bool VoiceCapture::muted() const noexcept { return _impl->muted; }
bool VoiceCapture::pollFrame(std::vector<float>& out) {
    auto& state = *_impl;
    if (!state.active || state.muted) return false;
    const auto frameCount = state.config.sampleRate / 50;
    if (state.ring.availableRead() < frameCount) return false;
    std::vector<float> candidate(static_cast<std::size_t>(frameCount) *
                                 state.config.channels);
    if (state.ring.read(candidate.data(), frameCount) != frameCount) return false;
    out = std::move(candidate);
    return true;
}
std::uint64_t VoiceCapture::droppedFrames() const noexcept { return _impl->dropped; }
std::uint32_t VoiceCapture::sampleRate() const noexcept { return _impl->config.sampleRate; }
std::uint8_t VoiceCapture::channels() const noexcept { return _impl->config.channels; }
bool VoiceCaptureTestAccess::beginBuffered(VoiceCapture& capture,
                                           const VoiceCaptureConfig& config) {
    capture.stop();
    if (!valid(config)) return false;
    auto& state = *capture._impl;
    if (!state.ring.initialize(config.channels, config.sampleRate / 2)) return false;
    state.config = config;
    state.dropped = 0;
    state.muted = false;
    state.active = true;
    return true;
}
void VoiceCaptureTestAccess::receive(VoiceCapture& capture, const float* pcm,
                                     std::uint32_t frames) {
    capture._impl->receive(pcm, frames);
}
} // namespace ayt::audio
