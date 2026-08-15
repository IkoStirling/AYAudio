#pragma once
// AYAudio/AudioEmitterSystem.h — World ISystem @ priority 600.
// Syncs AudioListenerComponent / AudioEmitterComponent + Transform → engine.

#include <AYEntity/IEntity.h>

namespace ayt::audio
{

class AudioEngine;

class AudioEmitterSystem : public ayt::entity::ISystem {
public:
    const char* getName() const override { return "AudioEmitterSystem"; }
    void onStart() override;
    void onUpdate(float dt) override;

    static constexpr int kPriority = 600;

    // Optional explicit bind (tests). When null, resolves via AudioSubSystem.
    void bindEngine(AudioEngine* engine) { _engine = engine; }

private:
    AudioEngine* resolveEngine() const;

    AudioEngine* _engine = nullptr;
};

void registerAudioEmitterSystem();

} // namespace ayt::audio
