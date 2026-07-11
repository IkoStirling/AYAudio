#include <AYAudioSubSystem.h>

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

bool AudioSubSystem::initialize() {
    _engine = std::make_unique<AudioEngine>();

    AudioSettings settings{};
    // Default to Null backend — caller may install a real backend via
    // AudioSubSystem::engine() before the first update(). AYApplication
    // chooses Miniaudio in production builds.
    settings.backend = AudioBackendKind::Null;
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.commandQueueCapacity = 256;
    settings.maxVoices = 32;
    settings.maxClips = 64;

    return _engine->initialize(settings);
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
}

} // namespace ayt::audio