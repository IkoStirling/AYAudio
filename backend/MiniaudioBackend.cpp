// MiniaudioBackend.cpp - Real device backend via miniaudio

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

void maDataCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/,
                    ma_uint32 frameCount) {
    if (pDevice == nullptr || pOutput == nullptr) {
        return;
    }
    auto* backend = static_cast<MiniaudioBackend*>(pDevice->pUserData);
    if (backend == nullptr) {
        const unsigned ch = pDevice->playback.channels;
        if (ch > 0) {
            std::memset(pOutput, 0,
                        static_cast<size_t>(frameCount) * ch * sizeof(float));
        }
        return;
    }
    backend->pumpFromEngine(static_cast<float*>(pOutput), frameCount);
}

} // namespace

MiniaudioBackend::MiniaudioBackend() = default;

MiniaudioBackend::~MiniaudioBackend() {
    stop();
}

void MiniaudioBackend::attachEngine(AudioEngine* engine) { _engine = engine; }

void MiniaudioBackend::pumpFromEngine(float* output, unsigned int frameCount) {
    if (_engine != nullptr) {
        _engine->render(output, frameCount);
        return;
    }
    if (output != nullptr && _device != nullptr && _device->playback.channels > 0) {
        std::memset(output, 0,
                    static_cast<size_t>(frameCount) * _device->playback.channels *
                        sizeof(float));
    }
}

bool MiniaudioBackend::start(const AudioBackendInfo& info) {
    if (_started) {
        return true;
    }

    _context = new (std::nothrow) ma_context();
    _device = new (std::nothrow) ma_device();
    if (_context == nullptr || _device == nullptr) {
        delete _context;
        delete _device;
        _context = nullptr;
        _device = nullptr;
        return false;
    }
    std::memset(_context, 0, sizeof(ma_context));
    std::memset(_device, 0, sizeof(ma_device));

    if (ma_context_init(nullptr, 0, nullptr, _context) != MA_SUCCESS) {
        delete _context;
        delete _device;
        _context = nullptr;
        _device = nullptr;
        return false;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = info.channels > 0 ? info.channels : 2;
    config.sampleRate        = info.sampleRate > 0 ? info.sampleRate : 48000;
    config.dataCallback      = maDataCallback;
    config.pUserData         = this;

    if (ma_device_init(_context, &config, _device) != MA_SUCCESS) {
        ma_context_uninit(_context);
        delete _context;
        delete _device;
        _context = nullptr;
        _device = nullptr;
        return false;
    }

    if (ma_device_start(_device) != MA_SUCCESS) {
        ma_device_uninit(_device);
        ma_context_uninit(_context);
        delete _device;
        delete _context;
        _device = nullptr;
        _context = nullptr;
        return false;
    }

    _info = info;
    _info.name = "Miniaudio";
    _info.realDevice = true;
    _info.sampleRate = _device->sampleRate;
    _info.channels = static_cast<uint16_t>(_device->playback.channels);
    _info.bitsPerSample = 32;
    _started = true;
    return true;
}

void MiniaudioBackend::stop() {
    if (!_started && _device == nullptr && _context == nullptr) {
        return;
    }
    _started = false;
    _engine = nullptr;

    if (_device != nullptr) {
        // Drop user data BEFORE stop/uninit so a late audio-thread callback
        // cannot re-enter this object or touch freed device state.
        _device->pUserData = nullptr;
        ma_device_stop(_device);
        ma_device_uninit(_device);
        delete _device;
        _device = nullptr;
    }
    if (_context != nullptr) {
        ma_context_uninit(_context);
        delete _context;
        _context = nullptr;
    }
    _info = AudioBackendInfo{};
}

void MiniaudioBackend::render(float*, uint32_t, uint32_t) {}

AudioBackendInfo MiniaudioBackend::describe() const { return _info; }

bool MiniaudioBackend::isRealDevice() const { return _started; }

std::unique_ptr<IAudioBackend> makeMiniaudioBackend() {
    return std::unique_ptr<IAudioBackend>(new MiniaudioBackend());
}

} // namespace ayt::audio
