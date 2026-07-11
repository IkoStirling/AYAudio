#include <AYAudio.h>
#include "AYTest.h"

using namespace ayt::audio;

namespace {

constexpr float kEps = 1e-4f;

ListenerParams defaultListener() {
    ListenerParams l{};
    l.position = ayt::math::FVector3{ 0.0f, 0.0f, 0.0f };
    l.forward  = ayt::math::FVector3{ 0.0f, 0.0f, -1.0f };
    l.up       = ayt::math::FVector3{ 0.0f, 1.0f, 0.0f };
    l.masterSpatialScale = 1.0f;
    return l;
}

SpatializationParams defaultVoice() {
    SpatializationParams v{};
    v.mode = SpatializationParams::Mode::Disabled;
    v.attenuation = SpatializationParams::AttenuationModel::InverseClamped;
    v.referenceDistance = 1.0f;
    v.maxDistance = 100.0f;
    v.stereoPan = 0.0f;
    v.spatialBlend = 0.0f;
    return v;
}

} // namespace

TEST_SUITE(AudioSpatializerTests)

    TEST_CASE(EqualPowerPanCenter) {
        float l = 0.0f, r = 0.0f;
        AudioSpatializer::equalPowerPan(0.0f, l, r);
        // Center: L = R = sin(π/4) = cos(π/4) ≈ 0.7071
        CHECK_FLOAT_EQ(l, 0.7071067f, kEps);
        CHECK_FLOAT_EQ(r, 0.7071067f, kEps);
    }

    TEST_CASE(EqualPowerPanFullLeft) {
        float l = 0.0f, r = 0.0f;
        AudioSpatializer::equalPowerPan(-1.0f, l, r);
        CHECK_FLOAT_EQ(l, 1.0f, kEps);
        CHECK_FLOAT_EQ(r, 0.0f, kEps);
    }

    TEST_CASE(EqualPowerPanFullRight) {
        float l = 0.0f, r = 0.0f;
        AudioSpatializer::equalPowerPan(1.0f, l, r);
        CHECK_FLOAT_EQ(l, 0.0f, kEps);
        CHECK_FLOAT_EQ(r, 1.0f, kEps);
    }

    TEST_CASE(EqualPowerPanPreservesTotalPower) {
        for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
            float l = 0.0f, r = 0.0f;
            AudioSpatializer::equalPowerPan(pan, l, r);
            const float total = l * l + r * r; // equal-power ⇒ constant 1
            CHECK_FLOAT_EQ(total, 1.0f, kEps);
        }
    }

    TEST_CASE(DistanceGainInverseClampedAtReference) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::InverseClamped;
        const float g = AudioSpatializer::distanceGain(v, v.referenceDistance);
        CHECK_FLOAT_EQ(g, 1.0f, kEps); // at ref ⇒ ref/ref * rolloff = 1
    }

    TEST_CASE(DistanceGainInverseClampedAtMax) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::InverseClamped;
        const float g = AudioSpatializer::distanceGain(v, v.maxDistance);
        // ref / max = 1/100 = 0.01
        CHECK_FLOAT_EQ(g, 0.01f, kEps);
    }

    TEST_CASE(DistanceGainLinearAtReference) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::Linear;
        const float g = AudioSpatializer::distanceGain(v, v.referenceDistance);
        CHECK_FLOAT_EQ(g, 1.0f, kEps);
    }

    TEST_CASE(DistanceGainLinearAtMax) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::Linear;
        const float g = AudioSpatializer::distanceGain(v, v.maxDistance);
        CHECK_FLOAT_EQ(g, 0.0f, kEps);
    }

    TEST_CASE(DistanceGainClampsToMinMax) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::InverseClamped;
        v.minGain = 0.4f;
        v.maxGain = 0.9f;
        // At reference distance the inverse-clamped gain would be 1.0,
        // which should clamp to maxGain.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, v.referenceDistance), 0.9f, kEps);
        // Very far should clamp up to minGain.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 1000.0f), 0.4f, kEps);
    }

    TEST_CASE(ComputeDisabledModeUsesStereoPan) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.mode = SpatializationParams::Mode::Disabled;
        v.stereoPan = -1.0f;
        SpatialGains g = AudioSpatializer::compute(l, v, 2);
        CHECK_FLOAT_EQ(g.leftGain, 1.0f, kEps);
        CHECK_FLOAT_EQ(g.rightGain, 0.0f, kEps);
    }

    TEST_CASE(ComputeWorldModeFrontHasEqualLAndR) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.mode = SpatializationParams::Mode::World;
        v.spatialBlend = 1.0f;
        v.position = ayt::math::FVector3{ 0.0f, 0.0f, -10.0f }; // in front
        SpatialGains g = AudioSpatializer::compute(l, v, 2);
        // Azimuth ≈ 0, equal-power ⇒ L ≈ R
        CHECK(std::fabs(g.leftGain - g.rightGain) < 0.05f);
    }

    TEST_CASE(ComputeWorldModeRightIsPanRight) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.mode = SpatializationParams::Mode::World;
        v.spatialBlend = 1.0f;
        v.position = ayt::math::FVector3{ 10.0f, 0.0f, 0.0f }; // due right
        SpatialGains g = AudioSpatializer::compute(l, v, 2);
        CHECK(g.rightGain > g.leftGain);
    }

    TEST_CASE(WorldDistanceAppliesSpatialScale) {
        ListenerParams l = defaultListener();
        l.masterSpatialScale = 2.0f;
        const float d = AudioSpatializer::worldDistance(
            l, ayt::math::FVector3{ 0.0f, 0.0f, 0.0f },
            ayt::math::FVector3{ 3.0f, 0.0f, 0.0f });
        // raw = 3.0, scaled = 6.0
        CHECK_FLOAT_EQ(d, 6.0f, kEps);
    }    
TEST_SUITE_END
