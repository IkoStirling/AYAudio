#pragma once
// AYAudioEmitterComponent.h — ECS one-shot / looping spatial sound source.
//
// Driven by AudioEmitterSystem (World priority 600). Requires a sibling
// Transform for world pose; falls back to spatial.position if absent.

#include <IAYEntity.h>
#include <AYAudioTypes.h>

namespace ayt::audio
{

#define AY_CURRENT_CLASS AudioEmitterComponent
struct AudioEmitterComponent : public ayt::entity::IComponent {
    const char* getName() const override { return "AudioEmitterComponent"; }

    AudioClipId clip = InvalidClipId;
    AudioBus bus = AudioBus::Sfx;
    SpatializationParams spatial{};
    float volume = 1.0f;
    bool loop = false;
    bool playOnStart = true;
    bool stopOnDetach = true;

    // Runtime (not authored):
    VoiceHandle voice = InvalidVoice;
    bool started = false;
    ayt::math::FVector3 lastPosition{};
    bool hasLastPosition = false;

    AudioEmitterComponent() {
        spatial.mode = SpatializationParams::Mode::World;
        spatial.spatialBlend = 1.0f;
    }
};
#undef AY_CURRENT_CLASS

} // namespace ayt::audio
