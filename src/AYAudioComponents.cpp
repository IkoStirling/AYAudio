#include <AYAudio/AudioComponents.h>

#include <AYAudio/components/AudioEmitterComponent.h>
#include <AYAudio/components/AudioListenerComponent.h>
#include <AYEntity/ComponentRegistration.h>

namespace ayt::audio
{

ayt::entity::ComponentRegistryResult registerAudioComponents(
    ayt::entity::ComponentRegistry& registry)
{
    ayt::entity::ComponentRegistryResult result =
        ayt::entity::registerComponent<AudioEmitterComponent>(
            registry,
            "AudioEmitterComponent",
            "Audio Emitter",
            "Audio");
    if (!result) {
        return result;
    }

    result = ayt::entity::registerComponent<AudioListenerComponent>(
        registry,
        "AudioListenerComponent",
        "Audio Listener",
        "Audio");
    if (!result) {
        return result;
    }

    return ayt::entity::ComponentRegistryResult::success();
}

void registerAudioComponents()
{
    (void)registerAudioComponents(
        ayt::entity::ComponentRegistry::instance());
}

} // namespace ayt::audio
