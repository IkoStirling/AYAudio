// AYAudioEmitterSystem.cpp — sync ECS listener/emitters to AudioEngine.

#include <AYAudioEmitterSystem.h>

#include <AYAudioEngine.h>
#include <AYAudioSubSystem.h>
#include <AYEntity.h>
#include <AYWorld.h>

#include <components/AYAudioEmitterComponent.h>
#include <components/AYAudioListenerComponent.h>
#include <components/AYTransformComponent.h>

#include <cstring>

namespace ayt::audio
{

namespace {

ayt::math::FVector3 forwardFromRotation(const ayt::math::FQuaternion& q) {
    // Local -Z is the engine's default "look" axis (ListenerParams.forward).
    return q * ayt::math::FVector3{0.0f, 0.0f, -1.0f};
}

ayt::math::FVector3 upFromRotation(const ayt::math::FQuaternion& q) {
    return q * ayt::math::FVector3{0.0f, 1.0f, 0.0f};
}

} // namespace

void AudioEmitterSystem::onStart() {
    // Engine may be bound later; nothing to do here.
}

AudioEngine* AudioEmitterSystem::resolveEngine() const {
    if (_engine != nullptr) {
        return _engine;
    }
    if (AudioSubSystem* sub = AudioSubSystem::findRegistered()) {
        return sub->engine();
    }
    return nullptr;
}

void AudioEmitterSystem::onUpdate(float dt) {
    AudioEngine* eng = resolveEngine();
    if (eng == nullptr || !eng->isInitialized()) {
        return;
    }

    ayt::entity::World& world = ayt::entity::World::instance();
    const float invDt = (dt > 1e-6f) ? (1.0f / dt) : 0.0f;

    // ---- Listener (first primary wins) ----
    bool listenerSet = false;
    for (ayt::entity::Entity* entity : world.query<AudioListenerComponent>()) {
        if (entity == nullptr) {
            continue;
        }
        AudioListenerComponent* listener = entity->getComponent<AudioListenerComponent>();
        if (listener == nullptr || !listener->isPrimary) {
            continue;
        }

        ListenerParams params{};
        params.hrtfEnabled = listener->hrtfEnabled;
        params.masterSpatialScale = listener->masterSpatialScale;

        if (ayt::entity::Transform* xf = entity->getComponent<ayt::entity::Transform>()) {
            params.position = xf->position;
            params.forward = forwardFromRotation(xf->rotation);
            params.up = upFromRotation(xf->rotation);
            if (listener->hasLastPosition && invDt > 0.0f) {
                params.velocity = (xf->position - listener->lastPosition) * invDt;
            }
            listener->lastPosition = xf->position;
            listener->hasLastPosition = true;
        }

        eng->setListener(params);
        listenerSet = true;
        break;
    }
    (void)listenerSet;

    // ---- Emitters ----
    for (ayt::entity::Entity* entity : world.query<AudioEmitterComponent>()) {
        if (entity == nullptr) {
            continue;
        }
        AudioEmitterComponent* emitter = entity->getComponent<AudioEmitterComponent>();
        if (emitter == nullptr) {
            continue;
        }

        SpatializationParams spatial = emitter->spatial;
        if (ayt::entity::Transform* xf = entity->getComponent<ayt::entity::Transform>()) {
            spatial.position = xf->position;
            spatial.forward = forwardFromRotation(xf->rotation);
            if (emitter->hasLastPosition && invDt > 0.0f) {
                spatial.velocity = (xf->position - emitter->lastPosition) * invDt;
            }
            emitter->lastPosition = xf->position;
            emitter->hasLastPosition = true;
        }

        if (!emitter->started && emitter->playOnStart && emitter->clip != InvalidClipId) {
            emitter->voice = eng->play(emitter->clip, emitter->bus, spatial,
                                       emitter->volume, emitter->loop);
            emitter->started = true;
        } else if (emitter->voice != InvalidVoice) {
            eng->setVoiceSpatial(emitter->voice, spatial);
            eng->setVoiceVolume(emitter->voice, emitter->volume);
        }
    }
}

void registerAudioEmitterSystem() {
    ayt::entity::World& world = ayt::entity::World::instance();
    if (world.findSystemByName("AudioEmitterSystem") != nullptr) {
        return;
    }
    world.registerSystem<AudioEmitterSystem>(AudioEmitterSystem::kPriority);
}

} // namespace ayt::audio
