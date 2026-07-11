#include <AYAudioSubSystem.h>

#include <AYAudioBackendFactory.h>
#include <AYAudioClipRegistry.h>

#include <IAYAudio.h>   // ayt::resource::IAudio

#include <cstdint>

namespace ayt::audio
{

namespace {

// Map AYResource's bits-per-sample to AYAudio's AudioSampleFormat enum.
// Only 16 and 32 are accepted: 8/24 would need a new engine dispatch path
// (out of scope for v1.1). Returning true means the input is supported;
// outFormat is set on success.
bool mapBitsPerSample(uint32_t bps, AudioSampleFormat& outFormat) {
    switch (bps) {
        case 16: outFormat = AudioSampleFormat::S16; return true;
        case 32: outFormat = AudioSampleFormat::F32; return true;
        default: return false;
    }
}

} // namespace

AudioSubSystem::AudioSubSystem() {
    _descriptor.name = "Audio";
    _descriptor.basePriority = 600; // see AYEntity/design.md: 600+ reserved for Audio
    _descriptor.timeType = ayt::game::SubSystemDescriptor::TimeType::Unscaled;
}

AudioSubSystem::~AudioSubSystem() {
    shutdown();
}

const ayt::game::SubSystemDescriptor& AudioSubSystem::getDescriptor() const {
    return _descriptor;
}

void AudioSubSystem::setBackend(std::unique_ptr<IAudioBackend> backend) {
    // The host installs a backend once, before initialize() runs. Once
    // initialized the engine already owns a backend; swapping here would
    // require coordinated shutdown — defer that to v2 if needed.
    if (_engine != nullptr && _engine->isInitialized()) {
        return;
    }
    _backend = std::move(backend);
}

AudioClipId AudioSubSystem::registerClipFromResource(const ayt::resource::IAudio* audio) {
    if (audio == nullptr) {
        return InvalidClipId;
    }
    if (_engine == nullptr || !_engine->isInitialized()) {
        return InvalidClipId;
    }

    AudioSampleFormat fmt = AudioSampleFormat::F32;
    if (!mapBitsPerSample(audio->getBitsPerSample(), fmt)) {
        return InvalidClipId;
    }

    const uint32_t channels = audio->getChannels();
    if (channels != 1 && channels != 2) {
        return InvalidClipId;
    }
    const uint64_t sampleCount = audio->getSampleCount();
    if (sampleCount == 0 || sampleCount > UINT32_MAX) {
        return InvalidClipId;
    }

    AudioClipDesc desc{};
    desc.sampleRate    = audio->getSampleRate();
    desc.channels      = static_cast<uint16_t>(channels);
    desc.bitsPerSample = static_cast<uint16_t>(audio->getBitsPerSample());
    desc.format        = fmt;
    // IAudio::getSampleCount returns total interleaved samples
    // (frames * channels). AudioClipDesc.frameCount is per-channel frames.
    desc.frameCount    = static_cast<uint32_t>(sampleCount / channels);
    desc.pcmData       = audio->getData();
    return _engine->registerClip(desc);
}

bool AudioSubSystem::initialize() {
    _engine = std::make_unique<AudioEngine>();

    AudioSettings settings{};
    // AudioSettings.backend is a hint; the engine reads the actual backend
    // from the unique_ptr installed via setBackend(). We mirror the choice
    // so debug introspection (engine.settings()) reflects reality.
    settings.backend = (_backend != nullptr) ? AudioBackendKind::Miniaudio
                                            : AudioBackendKind::Null;
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.commandQueueCapacity = 256;
    settings.maxVoices = 32;
    settings.maxClips = 64;

    // Hand the host-installed backend to the engine BEFORE initialize() so
    // AudioEngine::initialize picks it up (calls backend->start + attaches
    // the engine for MiniaudioBackend).
    if (_backend != nullptr) {
        _engine->setBackend(std::move(_backend));
    }

    if (!_engine->initialize(settings)) {
        _engine.reset();
        return false;
    }
    return true;
}

void AudioSubSystem::update(float deltaTime) {
    if (_engine) {
        _engine->submitFrame(deltaTime);
    }
}

void AudioSubSystem::fixedUpdate(float /*fixedDeltaTime*/) {
    // Audio playback is driven by the audio thread / backend callback, not by
    // fixed timestep. Nothing to do here in v0.5.
}

void AudioSubSystem::shutdown() {
    if (_engine) {
        _engine->shutdown();
        _engine.reset();
    }
    // If we never handed the backend to the engine (e.g. initialize() failed
    // before that step), make sure it gets stopped and dropped here.
    _backend.reset();
}

} // namespace ayt::audio