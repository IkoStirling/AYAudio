#pragma once
// AYAudioListenerComponent.h — ECS audio listener (usually the camera / player).
//
// AudioEmitterSystem picks the first primary listener each tick and pushes
// pose + hrtfEnabled into AudioEngine::setListener.

#include <IAYEntity.h>
#include <AYAudioTypes.h>

namespace ayt::audio
{

#define AY_CURRENT_CLASS AudioListenerComponent
struct AudioListenerComponent : public ayt::entity::IComponent {
    const char* getName() const override { return "AudioListenerComponent"; }

    bool isPrimary = true;
    bool hrtfEnabled = false;
    float masterSpatialScale = 1.0f;

    // Runtime velocity estimate from Transform delta.
    ayt::math::FVector3 lastPosition{};
    bool hasLastPosition = false;
};
#undef AY_CURRENT_CLASS

} // namespace ayt::audio
