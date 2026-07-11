// MiniaudioBackend.cpp - Real device backend via miniaudio (v0.5)
//
// This translation unit is the single place where miniaudio's implementation
// is emitted into the library (via MINIAUDIO_IMPLEMENTATION). All other TUs
// just see the public header.
//
// Wiring: AudioEngine::initialize() calls attachEngine(this) on the backend
// when it detects a MiniaudioBackend via dynamic_cast. The miniaudio data
// callback then drains AudioEngine::render() into the device-supplied
// interleaved float32 buffer. Anything that needs to flow through the audio
// thread (setBusGain, setListener, play/stop, etc.) goes via the engine's
// SPSC command queue, so the callback itself stays lock-free.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "MiniaudioBackend.h"

#include <cstring>
#include <memory>
#include <new>

namespace ayt::audio
{

namespace
{

// Thin wrapper so the miniaudio data callback (a free function with C
// linkage) can find our backend instance.
void maDataCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
    if (pDevice == nullptr || pOutput == nullptr) {
        return;
    }
    auto* backend = static_cast<MiniaudioBackend*>(pDevice->pUserData);
    if (backend == nullptr) {
        std::memset(pOutput, 0,
                    static_cast<size_t>(frameCount) * pDevice->playback.channels * sizeof(float));
        return;
    }
    backend->pumpFromEngine(static_cast<float*>(pOutput), frameCount);
}

} // namespace

MiniaudioBackend::MiniaudioBackend() = default;

MiniaudioBackend::~MiniaudioBackend() {
    stop();
    delete _device;
    _device = nullptr;
}

void MiniaudioBackend::attachEngine(AudioEngine* engine) { _engine = engine; }

void MiniaudioBackend::pumpFromEngine(float* output, unsigned int frameCount) {
    if (_engine != nullptr) {
        _engine->render(output, frameCount);
        return;
    }
    if (output != nullptr && _device != nullptr && _device->playback.channels > 0) {
        std::memset(output, 0,
                    static_cast<size_t>(frameCount) * _device->playback.channels * sizeof(float));
    }
}

bool MiniaudioBackend::start(const AudioBackendInfo& info) {
    if (_started) {
        return true;
    }

    _info = info;
    _info.name = "Miniaudio";
    _info.realDevice = true;

    if (_device == nullptr) {
        _device = new (std::nothrow) ma_device();
        if (_device == nullptr) {
            _info = AudioBackendInfo{};
            return false;
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = info.channels > 0 ? info.channels : 2;
    config.sampleRate        = info.sampleRate > 0 ? info.sampleRate : 48000;
    config.dataCallback      = maDataCallback;
    config.pUserData         = this;
    // Period sized for ~10 ms latency at the requested rate. miniaudio
    // picks a sane value if we leave it 0, but setting it explicitly keeps
    // tests / diagnostics predictable.
    config.periodSizeInFrames = config.sampleRate / 100;

    const ma_result result = ma_device_init(nullptr, &config, _device);
    if (result != MA_SUCCESS) {
        delete _device;
        _device = nullptr;
        _info = AudioBackendInfo{};
        return false;
    }

    const ma_result startResult = ma_device_start(_device);
    if (startResult != MA_SUCCESS) {
        ma_device_uninit(_device);
        delete _device;
        _device = nullptr;
        _info = AudioBackendInfo{};
        return false;
    }

    // Reflect what miniaudio actually opened (the backend may remap the
    // requested format).
    _info.sampleRate    = _device->sampleRate;
    _info.channels      = static_cast<uint16_t>(_device->playback.channels);
    _info.bitsPerSample = 32;

    _started = true;
    return true;
}

void MiniaudioBackend::stop() {
    if (!_started) {
        return;
    }
    if (_device != nullptr) {
        ma_device_stop(_device);
        ma_device_uninit(_device);
    }
    _started = false;
    _info = AudioBackendInfo{};
}

void MiniaudioBackend::render(float* /*output*/, uint32_t /*frameCount*/, uint32_t /*channels*/) {
    // MiniaudioBackend produces frames inside its own data callback via
    // pumpFromEngine(); we don't drive render() from the engine side.
}

AudioBackendInfo MiniaudioBackend::describe() const { return _info; }
bool MiniaudioBackend::isRealDevice() const { return _started; }

std::unique_ptr<IAudioBackend> makeMiniaudioBackend() {
    return std::unique_ptr<IAudioBackend>(new MiniaudioBackend());
}

} // namespace ayt::audio