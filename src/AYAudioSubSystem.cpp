#include <AYAudioSubSystem.h>

#include <AYAudioBackendFactory.h>

namespace ayt::audio
{

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