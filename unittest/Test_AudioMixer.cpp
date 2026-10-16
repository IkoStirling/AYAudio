#include <AYAudio.h>
#include "AYTest.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
        CHECK(!mixer.initialize(8, 3, 48000)); // not 1 or 2
        CHECK(mixer.initialize(8, 1, 48000));   // mono OK
        CHECK(mixer.initialize(8, 2, 48000));   // stereo OK
    }

    TEST_CASE(MixerInitialGainIsUnity) {
        AudioMixer mixer;
        mixer.initialize(8, 2, 48000);
        CHECK_FLOAT_EQ(mixer.getMasterGain(), 1.0f, 1e-6f);
        CHECK_FLOAT_EQ(mixer.getBusGain(AudioBus::Sfx), 1.0f, 1e-6f);
    }

    TEST_CASE(MixerAppliesBusGainAndMasterGain) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
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
        mixer.initialize(4, 2, 48000);
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
        mixer.initialize(4, 2, 48000);
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
        mixer.initialize(4, 2, 48000);
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
        mixer.initialize(4, 2, 48000);
        mixer.setBusGain(AudioBus::Music, 0.1f);
        mixer.setMasterGain(0.2f);
        mixer.reset();
        CHECK_FLOAT_EQ(mixer.getBusGain(AudioBus::Music), 1.0f, 1e-6f);
        CHECK_FLOAT_EQ(mixer.getMasterGain(), 1.0f, 1e-6f);
    }

    // ===== v2.0b: Ducking =====

    TEST_CASE(DuckingInactiveWhenNoVoiceBusVoice) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        mixer.setDucking(-12.0f, 8.0f, 2.0f);
        // No voices on Voice bus → envelope target stays 0.
        // Build a Sfx-bus voice.
        AudioMixer::VoiceSourceFn src = &constantSource;
        VoiceRuntime v{};
        v.bus = AudioBus::Sfx;
        v.volume = 1.0f;
        void* ud = nullptr;
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        mixer.mix(output, kFrames, &v, &src, &ud, 1);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);
        CHECK_FLOAT_EQ(mixer.duckState().currentDb, 0.0f, 1e-6f);
    }

    TEST_CASE(DuckingEngagesWhenVoiceBusActive) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        mixer.setDucking(-12.0f, 100.0f, 100.0f);  // fast ramp
        AudioMixer::VoiceSourceFn src = &constantSource;
        VoiceRuntime v{};
        v.bus = AudioBus::Voice;
        v.volume = 1.0f;
        void* ud = nullptr;
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        mixer.mix(output, kFrames, &v, &src, &ud, 1);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);
        // After 64/48000 ≈ 1.33 ms @ attack 100 dB/s ⇒ -0.13 dB.
        CHECK(mixer.duckState().currentDb < -0.05f);
    }

    TEST_CASE(DuckingReleasesAfterVoiceStops) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        mixer.setDucking(-12.0f, 1000.0f, 1000.0f);
        // Engage.
        AudioMixer::VoiceSourceFn src = &constantSource;
        VoiceRuntime voice{};
        voice.bus = AudioBus::Voice;
        voice.volume = 1.0f;
        void* ud = nullptr;
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        mixer.mix(output, kFrames, &voice, &src, &ud, 1);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);
        // Now release — render with no voices.
        mixer.mix(output, kFrames, nullptr, nullptr, nullptr, 0);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);
        CHECK(mixer.duckState().currentDb > -11.0f); // released some.
    }

    TEST_CASE(DuckingAttackRateMatchesSettings) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        mixer.setDucking(-12.0f, 1000.0f, 1000.0f);
        AudioMixer::VoiceSourceFn src = &constantSource;
        VoiceRuntime v{};
        v.bus = AudioBus::Voice;
        v.volume = 1.0f;
        void* ud = nullptr;
        constexpr uint32_t kFrames = 480;  // 10 ms @ 48 kHz
        float output[kFrames * 2]{};
        mixer.mix(output, kFrames, &v, &src, &ud, 1);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);
        // 10 ms × 1000 dB/s = -10 dB (attack ramp).
        CHECK_FLOAT_EQ(mixer.duckState().currentDb, -10.0f, 1e-3f);
    }

    TEST_CASE(DuckingAppliesToMusicBusOnly) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        mixer.setDucking(-60.0f, 1000.0f, 1000.0f); // deep ducking
        AudioMixer::VoiceSourceFn src = &constantSource;
        VoiceRuntime voice{};
        voice.bus = AudioBus::Voice;
        voice.volume = 1.0f;
        void* ud = nullptr;
        // Engage ducking with Voice bus active (~48 ms → ~-48 dB).
        constexpr uint32_t kFrames = 2304;
        float outA[64 * 2]{};
        mixer.mix(outA, 64, &voice, &src, &ud, 1);
        mixer.integrateDuckEnvelope(static_cast<float>(kFrames) / 48000.0f);

        VoiceRuntime sfxVoice{};
        sfxVoice.bus = AudioBus::Sfx;
        sfxVoice.volume = 1.0f;
        VoiceRuntime musicVoice{};
        musicVoice.bus = AudioBus::Music;
        musicVoice.volume = 1.0f;
        float outSfx[64 * 2]{};
        float outMusic[64 * 2]{};
        mixer.mix(outSfx, 64, &sfxVoice, &src, &ud, 1);
        mixer.mix(outMusic, 64, &musicVoice, &src, &ud, 1);
        // Sfx is not ducked; Music follows the duck envelope.
        CHECK(outSfx[0] > 0.9f);
        CHECK(outSfx[1] > 0.9f);
        CHECK(outMusic[0] < 0.05f);
        CHECK(outMusic[1] < 0.05f);
    }

    // ===== v2.0b: Reverb send =====

    TEST_CASE(ReverbInactiveWhenNoSendAndNoVoice) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        // No voices → output is silence; reverb contributes nothing.
        float output[8] = { 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f };
        mixer.mix(output, 4, nullptr, nullptr, nullptr, 0);
        for (int i = 0; i < 8; ++i) {
            CHECK_FLOAT_EQ(output[i], 0.0f, 1e-6f);
        }
    }

    TEST_CASE(ReverbTailImpulse) {
        AudioMixer mixer;
        mixer.initialize(4, 2, 48000);
        // A single-frame impulse via custom source.
        struct Impulse {
            static void fire(void*, uint32_t, float* buf, uint32_t frames) {
                for (uint32_t f = 0; f < frames * 2; ++f) buf[f] = 0.0f;
                buf[0] = 1.0f; buf[1] = 1.0f;
            }
        };
        AudioMixer::VoiceSourceFn src = &Impulse::fire;
        VoiceRuntime v{};
        v.bus = AudioBus::Sfx;
        v.volume = 1.0f;
        v.spatial.reverbSend = 1.0f;
        void* ud = nullptr;
        // Render one block with the impulse.
        constexpr uint32_t kFrames = 64;
        float output[kFrames * 2]{};
        mixer.mix(output, kFrames, &v, &src, &ud, 1);
        // Render 2 seconds of silence to drain the tail; first frames of
        // output should still be non-zero (the reverb tail).
        // Heap allocation: ~768 KiB exceeds typical default stack budgets.
        constexpr uint32_t kTail = 96000;
        std::vector<float> tail(static_cast<size_t>(kTail) * 2u, 0.0f);
        for (uint32_t i = 0; i < (kTail / kFrames); ++i) {
            mixer.mix(tail.data() + (i * kFrames * 2), kFrames, nullptr, nullptr, nullptr, 0);
        }
        // Peak across the tail (excluding the impulse frame itself).
        float peak = 0.0f;
        for (uint32_t f = 1; f < kTail; ++f) {
            peak = std::max(peak, std::fabs(tail[2 * f + 0]));
            peak = std::max(peak, std::fabs(tail[2 * f + 1]));
        }
        CHECK(peak > 0.001f);
    }

    TEST_CASE(ReverbReturnScalesWetLevel) {
        // A voice with reverbSend=1.0 + different master returns should
        // produce different wet peaks on the second block (after the impulse).
        auto runPeak = [](float masterReturn) {
            AudioMixer mixer;
            mixer.initialize(4, 2, 48000);
            mixer.setMasterReverbReturn(masterReturn);
            struct Impulse {
                static void fire(void*, uint32_t, float* buf, uint32_t frames) {
                    for (uint32_t f = 0; f < frames * 2; ++f) buf[f] = 0.0f;
                    buf[0] = 1.0f; buf[1] = 1.0f;
                }
            };
            AudioMixer::VoiceSourceFn src = &Impulse::fire;
            VoiceRuntime v{};
            v.bus = AudioBus::Sfx;
            v.volume = 1.0f;
            v.spatial.reverbSend = 1.0f;
            void* ud = nullptr;
            constexpr uint32_t kFrames = 64;
            float output[kFrames * 2]{};
            mixer.mix(output, kFrames, &v, &src, &ud, 1);
            // Comb delays are ~30–70 ms; drain past that so wet energy appears.
            constexpr uint32_t kTail = 8192;
            std::vector<float> tail(static_cast<size_t>(kTail) * 2u, 0.0f);
            for (uint32_t i = 0; i < (kTail / kFrames); ++i) {
                mixer.mix(tail.data() + (i * kFrames * 2), kFrames,
                          nullptr, nullptr, nullptr, 0);
            }
            float peak = 0.0f;
            for (uint32_t i = 0; i < kTail * 2; ++i) {
                peak = std::max(peak, std::fabs(tail[i]));
            }
            return peak;
        };
        const float peakHi = runPeak(1.0f);
        const float peakLo = runPeak(0.25f);
        CHECK(peakHi > peakLo * 2.0f);   // 4× return ⇒ ~4× wet peak.
    }

    TEST_CASE(ReverbPresetSwitchesTailLength) {
        // Preset 1 (hall) has higher comb feedback → energy decays slower.
        auto integrateTailEnergy = [](uint8_t preset) {
            AudioMixer mixer;
            mixer.initialize(4, 2, 48000);
            mixer.setReverbPreset(preset);
            mixer.setMasterReverbReturn(1.0f);
            struct Impulse {
                static void fire(void*, uint32_t, float* buf, uint32_t frames) {
                    for (uint32_t f = 0; f < frames * 2; ++f) buf[f] = 0.0f;
                    buf[0] = 1.0f; buf[1] = 1.0f;
                }
            };
            AudioMixer::VoiceSourceFn src = &Impulse::fire;
            VoiceRuntime v{};
            v.bus = AudioBus::Sfx;
            v.volume = 1.0f;
            v.spatial.reverbSend = 1.0f;
            void* ud = nullptr;
            constexpr uint32_t kFrames = 64;
            float output[kFrames * 2]{};
            mixer.mix(output, kFrames, &v, &src, &ud, 1);
            // Drain 0.5s of silence; sum |output| as energy.
            // Heap allocation: large VLA-style stack buffers overflow on MSVC.
            constexpr uint32_t kTail = 24000;
            std::vector<float> tail(static_cast<size_t>(kTail) * 2u, 0.0f);
            for (uint32_t i = 0; i < (kTail / kFrames); ++i) {
                mixer.mix(tail.data() + (i * kFrames * 2), kFrames, nullptr, nullptr, nullptr, 0);
            }
            double energy = 0.0;
            for (uint32_t i = 0; i < kTail * 2; ++i) {
                energy += static_cast<double>(std::fabs(tail[i]));
            }
            return energy;
        };
        const double e0 = integrateTailEnergy(0);
        const double e1 = integrateTailEnergy(1);
        // Hall preset (longer feedback, higher gain) should leave more
        // energy in the tail than the small-room preset.
        CHECK(e1 > e0);
    }
TEST_CASE(BusPeaksMeasureSummedDrySignalAndResetOnSilentBlock) {
    AudioMixer mixer;CHECK(mixer.initialize(4,2,48000));
    VoiceRuntime voices[2];voices[0].bus=voices[1].bus=AudioBus::Sfx;
    AudioMixer::VoiceSourceFn sources[2]={constantSource,constantSource};void* users[2]={};float output[16]{};
    mixer.setBusGain(AudioBus::Sfx,.5f);mixer.mix(output,8,voices,sources,users,2);
    CHECK_FLOAT_EQ(mixer.busPeaks()[size_t(AudioBus::Sfx)],1.f,1e-6f);
    voices[1].volume=-1; // Diagnostic sums must preserve cancellation before taking a peak.
    mixer.mix(output,8,voices,sources,users,2);CHECK_FLOAT_EQ(mixer.busPeaks()[size_t(AudioBus::Sfx)],0.f,1e-6f);
    mixer.mix(output,8,nullptr,nullptr,nullptr,0);CHECK_FLOAT_EQ(mixer.busPeaks()[size_t(AudioBus::Sfx)],0.f,1e-6f);
    voices[1].volume=1; mixer.mix(output,8,voices,sources,users,2);
    mixer.reset(); CHECK(mixer.busPeaks()[size_t(AudioBus::Sfx)]==0); CHECK(mixer.outputLevels().left==0);
}
TEST_SUITE_END
