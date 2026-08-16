#include <AYAudio.h>
#include <AYEntity.h>
#include <AYEntity/World.h>
#include <AYEntity/components/TransformComponent.h>

#include "AYTest.h"

#include <vector>

using namespace ayt::audio;
using namespace ayt::entity;

namespace {

AudioSettings defaultSettings() {
    AudioSettings s{};
    s.backend = AudioBackendKind::Null;
    s.sampleRate = 48000;
    s.channels = 2;
    s.commandQueueCapacity = 32;
    s.maxVoices = 8;
    s.maxClips = 8;
    return s;
}

std::vector<float> g_pcm;

AudioClipId registerTone(AudioEngine& engine) {
    g_pcm.assign(256, 0.4f);
    AudioClipDesc desc{};
    desc.sampleRate = 48000;
    desc.channels = 1;
    desc.format = AudioSampleFormat::F32;
    desc.frameCount = 256;
    desc.pcmData = g_pcm.data();
    return engine.registerClip(desc);
}

} // namespace

TEST_SUITE(AudioEmitterSystemTests)

    TEST_CASE(EmitterPlayOnStartAndListenerPose) {
        World& world = World::instance();
        world.shutdown();
        CHECK(world.initialize());

        AudioEngine engine;
        CHECK(engine.initialize(defaultSettings()));
        const AudioClipId clip = registerTone(engine);
        CHECK(clip != InvalidClipId);

        registerAudioEmitterSystem();
        auto* sys = static_cast<AudioEmitterSystem*>(
            world.findSystemByName("AudioEmitterSystem"));
        CHECK(sys != nullptr);
        sys->bindEngine(&engine);

        Entity* listenerEnt = world.createEntity();
        auto* listener = listenerEnt->addComponent<AudioListenerComponent>();
        auto* listenerXf = listenerEnt->addComponent<Transform>();
        listener->isPrimary = true;
        listener->hrtfEnabled = true;
        listenerXf->position = {1.0f, 2.0f, 3.0f};

        Entity* emitterEnt = world.createEntity();
        auto* emitter = emitterEnt->addComponent<AudioEmitterComponent>();
        auto* emitterXf = emitterEnt->addComponent<Transform>();
        emitter->clip = clip;
        emitter->playOnStart = true;
        emitter->loop = true;
        emitterXf->position = {10.0f, 0.0f, 0.0f};

        world.update(1.0f / 60.0f);

        CHECK(emitter->started);
        CHECK(emitter->voice != InvalidVoice);
        CHECK_FLOAT_EQ(engine.listener().position.x, 1.0f, 1e-4f);
        CHECK_FLOAT_EQ(engine.listener().position.y, 2.0f, 1e-4f);
        CHECK_FLOAT_EQ(engine.listener().position.z, 3.0f, 1e-4f);
        CHECK(engine.listener().hrtfEnabled);

        bool found = false;
        for (uint32_t i = 0; i < engine.settings().maxVoices; ++i) {
            const VoiceRuntime* vr = engine.voiceAt(i);
            if (vr && vr->playing && vr->clip == clip) {
                CHECK_FLOAT_EQ(vr->spatial.position.x, 10.0f, 1e-4f);
                found = true;
                break;
            }
        }
        CHECK(found);

        engine.shutdown();
        world.shutdown();
    }

TEST_SUITE_END
