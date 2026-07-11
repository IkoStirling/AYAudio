#include <AYAudio.h>
#include "AYTest.h"

using namespace ayt::audio;

namespace {

// Constant-source callback: writes 1.0 into every sample of voiceBuffer.
// Used to verify mixer gain math in isolation.
void constantSource(void* /*userData*/, uint32_t /*voiceIndex*/,
                    float* voiceBuffer, uint32_t frameCount) {
    for (uint32_t f = 0; f < frameCount * 2; ++f) {
        voiceBuffer[f] = 1.0f;
    }
}

void silentSource(void* /*userData*/, uint32_t /*voiceIndex*/,
                  float* voiceBuffer, uint32_t frameCount) {
    for (uint32_t f = 0; f < frameCount * 2; ++f) {
        voiceBuffer[f] = 0.0f;
    }
}

} // namespace

TEST_SUITE(AudioMixerTests)

    TEST_CASE(MixerRejectsInvalidChannelCount) {
        AudioMixer mixer;
        CHECK(!mixer.initialize(8, 3)); // not 1 or 2
        CHECK(mixer.initialize(8, 1));   // mono OK
        CHECK(mixer.initialize(8, 2));   // stereo OK
    }

    TEST_CASE(MixerInitialGainIsUnity) {
        AudioMixer mixer;
        mixer.initialize(8, 2);
        CHECK_FLOAT_EQ(mixer.getMasterGain(), 1.0f, 1e-6f);
        CHECK_FLOAT_EQ(mixer.getBusGain(AudioBus::Sfx), 1.0f, 1e-6f);
    }

    TEST_CASE(MixerAppliesBusGainAndMasterGain) {
        AudioMixer mixer;
        mixer.initialize(4, 2);
        mixer.setBusGain(AudioBus::Sfx, 0.5f);
        mixer.setMasterGain(0.25f);

        VoiceRuntime v{};
        v.bus = AudioBus::Sfx;
        v.volume = 1.0f;

        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        AudioMixer::VoiceSourceFn sources[1] = { &constantSource };
        void* ud[1] = { nullptr };
        mixer.mix(output, kFrames, &v, sources, ud, 1);

        // Expected: 1.0 (source) * 1.0 (voice) * 0.5 (bus) * 0.25 (master)
        const float expected = 0.5f * 0.25f;
        for (uint32_t f = 0; f < kFrames; ++f) {
            CHECK_FLOAT_EQ(output[2 * f + 0], expected, 1e-6f);
            CHECK_FLOAT_EQ(output[2 * f + 1], expected, 1e-6f);
        }
    }

    TEST_CASE(MixerClampsSaturatedOutput) {
        AudioMixer mixer;
        mixer.initialize(4, 2);
        mixer.setBusGain(AudioBus::Sfx, 1.0f);
        mixer.setMasterGain(1.0f);

        VoiceRuntime voices[3]{};
        for (int i = 0; i < 3; ++i) {
            voices[i].bus = AudioBus::Sfx;
            voices[i].volume = 1.0f;
        }

        constexpr uint32_t kFrames = 16;
        float output[kFrames * 2]{};
        AudioMixer::VoiceSourceFn sources[3] = { &constantSource, &constantSource, &constantSource };
        void* ud[3] = { nullptr, nullptr, nullptr };
        mixer.mix(output, kFrames, voices, sources, ud, 3);

        // 3 voices at full gain → 3.0 each frame → clamps to 1.0
        for (uint32_t f = 0; f < kFrames; ++f) {
            CHECK_FLOAT_EQ(output[2 * f + 0], 1.0f, 1e-6f);
            CHECK_FLOAT_EQ(output[2 * f + 1], 1.0f, 1e-6f);
        }
    }

    TEST_CASE(MixerZeroesOutputWhenNoVoices) {
        AudioMixer mixer;
        mixer.initialize(4, 2);
        mixer.setBusGain(AudioBus::Sfx, 0.8f);

        constexpr uint32_t kFrames = 8;
        // Pre-fill the output with a sentinel; mixer should overwrite to zero
        // (no voices) and not touch the sentinels with garbage.
        float output[kFrames * 2];
        for (uint32_t i = 0; i < kFrames * 2; ++i) output[i] = 99.0f;
        mixer.mix(output, kFrames, nullptr, nullptr, nullptr, 0);
        for (uint32_t i = 0; i < kFrames * 2; ++i) {
            CHECK_FLOAT_EQ(output[i], 0.0f, 1e-6f);
        }
    }

    TEST_CASE(MixerSilentVoiceProducesSilence) {
        AudioMixer mixer;
        mixer.initialize(4, 2);
        VoiceRuntime v{};
        v.bus = AudioBus::Sfx;
        v.volume = 1.0f;

        constexpr uint32_t kFrames = 16;
        float output[kFrames * 2]{};
        AudioMixer::VoiceSourceFn sources[1] = { &silentSource };
        void* ud[1] = { nullptr };
        mixer.mix(output, kFrames, &v, sources, ud, 1);
        for (uint32_t i = 0; i < kFrames * 2; ++i) {
            CHECK_FLOAT_EQ(output[i], 0.0f, 1e-6f);
        }
    }

    TEST_CASE(MixerResetRestoresUnityGains) {
        AudioMixer mixer;
        mixer.initialize(4, 2);
        mixer.setBusGain(AudioBus::Music, 0.1f);
        mixer.setMasterGain(0.2f);
        mixer.reset();
        CHECK_FLOAT_EQ(mixer.getBusGain(AudioBus::Music), 1.0f, 1e-6f);
        CHECK_FLOAT_EQ(mixer.getMasterGain(), 1.0f, 1e-6f);
    }    
TEST_SUITE_END
