#include <AYAudio.h>
#include "AYTest.h"

#include <cmath>

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

    // ===== coneGain =====

    TEST_CASE(ConeGainOmnidirectionalAlwaysFull) {
        // innerConeAngleDeg defaults to 360 ⇒ no cone attenuation.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.innerConeAngleDeg = 360.0f;
        v.outerConeAngleDeg = 360.0f;
        v.outerConeGain = 0.0f;
        // Source directly behind listener: still full gain when omnidirectional.
        v.position = ayt::math::FVector3{ 0.0f, 0.0f, 100.0f };
        v.forward = ayt::math::FVector3{ 0.0f, 0.0f, -1.0f };
        CHECK_FLOAT_EQ(AudioSpatializer::coneGain(l, v), 1.0f, kEps);
    }

    TEST_CASE(ConeGainInsideInnerConeIsOne) {
        // Source lies on the cone's axis (in front of listener when
        // forward == (0,0,-1)): angle ≈ 0 ⇒ inside inner cone.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.innerConeAngleDeg = 60.0f;
        v.outerConeAngleDeg = 120.0f;
        v.outerConeGain = 0.2f;
        v.position = ayt::math::FVector3{ 0.0f, 0.0f, -10.0f };
        v.forward = ayt::math::FVector3{ 0.0f, 0.0f, -1.0f };
        CHECK_FLOAT_EQ(AudioSpatializer::coneGain(l, v), 1.0f, kEps);
    }

    TEST_CASE(ConeGainOutsideOuterConeIsOuterGain) {
        // Source is fully to the side (90° off-axis). Outer cone is 60°, so
        // the source sits well outside and we get outerConeGain.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.innerConeAngleDeg = 30.0f;
        v.outerConeAngleDeg = 60.0f;
        v.outerConeGain = 0.25f;
        v.position = ayt::math::FVector3{ 10.0f, 0.0f, 0.0f }; // 90° from forward
        v.forward = ayt::math::FVector3{ 0.0f, 0.0f, -1.0f };
        CHECK_FLOAT_EQ(AudioSpatializer::coneGain(l, v), 0.25f, kEps);
    }

    TEST_CASE(ConeGainInterpolatesBetweenInnerAndOuter) {
        // Place the source exactly halfway between the inner (45°) and outer
        // (90°) cones. Expect the gain to be halfway between 1.0 and 0.2.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.innerConeAngleDeg = 45.0f;
        v.outerConeAngleDeg = 90.0f;
        v.outerConeGain = 0.2f;

        // Place source so the angle from voice.forward is ≈ 67.5°.
        // voice.forward = (0, 0, -1). Place source at (sin(67.5°), 0, -cos(67.5°)).
        const float a = 67.5f * 0.017453292519943f; // deg→rad
        v.position = ayt::math::FVector3{ std::sin(a), 0.0f, -std::cos(a) };
        v.forward = ayt::math::FVector3{ 0.0f, 0.0f, -1.0f };

        const float expected = 0.5f * 1.0f + 0.5f * 0.2f; // = 0.6
        CHECK_FLOAT_EQ(AudioSpatializer::coneGain(l, v), expected, 1e-3f);
    }

    TEST_CASE(ConeGainDegenerateForwardIsOne) {
        // Zero-length forward ⇒ we can't define a cone; default to full gain
        // so we don't accidentally silence the voice.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.innerConeAngleDeg = 60.0f;
        v.outerConeAngleDeg = 120.0f;
        v.outerConeGain = 0.0f;
        v.position = ayt::math::FVector3{ 1.0f, 0.0f, 0.0f };
        v.forward = ayt::math::FVector3{ 0.0f, 0.0f, 0.0f };
        CHECK_FLOAT_EQ(AudioSpatializer::coneGain(l, v), 1.0f, kEps);
    }

    // ===== occlusionGain =====

    TEST_CASE(OcclusionGainClearIsUnity) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 0.0f;
        v.obstruction = 0.0f;
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 1.0f, kEps);
    }

    TEST_CASE(OcclusionGainFullOcclusionIsZero) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 1.0f;
        v.obstruction = 0.0f;
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 0.0f, kEps);
    }

    TEST_CASE(OcclusionGainFullObstructionIsHalf) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 0.0f;
        v.obstruction = 1.0f;
        // Obstruction is half-gain (0.5), occlusion is none (1.0).
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 0.5f, kEps);
    }

    TEST_CASE(OcclusionGainHalfOcclusion) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 0.5f;
        v.obstruction = 0.0f;
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 0.5f, kEps);
    }

    TEST_CASE(OcclusionGainCombinedMultiplies) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 0.5f;
        v.obstruction = 0.5f;
        // 0.5 * (1 - 0.25) = 0.375
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 0.375f, kEps);
    }

    TEST_CASE(OcclusionGainClampsInputs) {
        SpatializationParams v = defaultVoice();
        v.occlusion = 2.0f;     // out of range
        v.obstruction = -0.5f;  // out of range
        CHECK_FLOAT_EQ(AudioSpatializer::occlusionGain(v), 0.0f, kEps);
    }

    // ===== compute() integration =====

    TEST_CASE(ComputeWorldModeAppliesOcclusionToMono) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.mode = SpatializationParams::Mode::World;
        v.spatialBlend = 1.0f;
        v.position = ayt::math::FVector3{ 0.0f, 0.0f, -2.0f };
        v.occlusion = 0.5f;
        v.obstruction = 0.0f;
        // monoGain reports the per-voice attenuation including occlusion.
        // Distance attenuation at d=2 with ref=1 (InverseClamped) gives
        // 1.0/2.0 = 0.5; then occlusion 0.5 multiplies in ⇒ 0.25.
        SpatialGains g = AudioSpatializer::compute(l, v, 1);
        CHECK_FLOAT_EQ(g.monoGain, 0.25f, 1e-3f);
    }

    TEST_CASE(ComputeDisabledModeIgnoresConeAndOcclusion) {
        // Disabled mode returns the 2D pan only; cone/occlusion fields must
        // not influence the result.
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.mode = SpatializationParams::Mode::Disabled;
        v.stereoPan = 0.0f;
        v.spatialBlend = 0.0f;
        v.occlusion = 1.0f;       // would silence in World mode
        v.obstruction = 1.0f;
        v.outerConeGain = 0.0f;
        SpatialGains g = AudioSpatializer::compute(l, v, 2);
        // Disabled: equal-power pan, no occlusion applied.
        CHECK_FLOAT_EQ(g.leftGain, 0.7071067f, kEps);
        CHECK_FLOAT_EQ(g.rightGain, 0.7071067f, kEps);
    }

    // ===== v2.0b: CustomCurve attenuation =====

    TEST_CASE(CustomCurveEmptyFallsBackToInverseClamped) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::CustomCurve;
        v.customCurveCount = 0;
        v.referenceDistance = 1.0f;
        v.maxDistance = 100.0f;
        // InverseClamped fallback: ref / max(d, ref) * rolloff.
        const float expected = 1.0f / std::max(0.0001f, 10.0f);  // d=10, ref=1
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 10.0f), expected, kEps);
    }

    TEST_CASE(CustomCurveSingleSampleIsConstant) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::CustomCurve;
        v.customCurveCount = 1;
        v.customCurve[0] = { 5.0f, 0.7f };
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 0.0f),  0.7f, kEps);
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 50.0f), 0.7f, kEps);
    }

    TEST_CASE(CustomCurveInterpolatesBetweenSamples) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::CustomCurve;
        v.referenceDistance = 0.0f;
        v.maxDistance = 1.0f;
        v.customCurveCount = 2;
        v.customCurve[0] = { 0.0f, 1.0f };
        v.customCurve[1] = { 1.0f, 0.5f };
        // Mid-distance → midpoint of [1.0, 0.5] = 0.75.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 0.5f), 0.75f, kEps);
    }

    TEST_CASE(CustomCurveClampsAtEdges) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::CustomCurve;
        v.referenceDistance = 0.0f;
        v.maxDistance = 10.0f;
        v.customCurveCount = 3;
        v.customCurve[0] = { 1.0f, 0.9f };
        v.customCurve[1] = { 5.0f, 0.5f };
        v.customCurve[2] = { 9.0f, 0.2f };
        // distance < ref → first sample's gain.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, -3.0f), 0.9f, kEps);
        // distance > max → last sample's gain.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 100.0f), 0.2f, kEps);
    }

    TEST_CASE(CustomCurveClampsInputsToRefMax) {
        SpatializationParams v = defaultVoice();
        v.attenuation = SpatializationParams::AttenuationModel::CustomCurve;
        v.referenceDistance = 10.0f;
        v.maxDistance = 50.0f;
        v.customCurveCount = 3;
        v.customCurve[0] = { 10.0f, 0.9f };
        v.customCurve[1] = { 30.0f, 0.5f };
        v.customCurve[2] = { 50.0f, 0.2f };
        // Out-of-range distance clamps to 50 → 0.2.
        CHECK_FLOAT_EQ(AudioSpatializer::distanceGain(v, 200.0f), 0.2f, kEps);
    }

    // ===== v2.0b: dopplerFactor =====

    TEST_CASE(DopplerStaticListenerAndSourceIsUnity) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 10.0f, 0.0f, 0.0f };
        v.velocity = { 0.0f, 0.0f, 0.0f };
        l.velocity = { 0.0f, 0.0f, 0.0f };
        CHECK_FLOAT_EQ(AudioSpatializer::dopplerFactor(l, v), 1.0f, 1e-4f);
    }

    TEST_CASE(DopplerSourceMovingTowardRaisesPitch) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 10.0f, 0.0f, 0.0f };
        // Source moving in -X (toward listener at origin).
        v.velocity = { -5.0f, 0.0f, 0.0f };
        l.velocity = { 0.0f, 0.0f, 0.0f };
        const float f = AudioSpatializer::dopplerFactor(l, v);
        CHECK(f > 1.0f);
    }

    TEST_CASE(DopplerListenerMovingTowardRaisesPitch) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 10.0f, 0.0f, 0.0f };
        v.velocity = { 0.0f, 0.0f, 0.0f };
        // Listener moving in +X (toward static source at +10) → higher pitch.
        l.velocity = { 5.0f, 0.0f, 0.0f };
        const float f = AudioSpatializer::dopplerFactor(l, v);
        CHECK(f > 1.0f);
    }

    TEST_CASE(DopplerClampsAtMaxRatio) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 10.0f, 0.0f, 0.0f };
        v.velocity = { 0.0f, 0.0f, 0.0f };
        // Extreme listener velocity ⇒ ratio hits floor (0.5) ⇒ factor ceiling (2.0).
        l.velocity = { 1.0e6f, 0.0f, 0.0f };
        const float f = AudioSpatializer::dopplerFactor(l, v);
        CHECK(f <= 2.0f + 1e-4f);
        CHECK(f >= 0.5f - 1e-4f);
    }

    TEST_CASE(DopplerClampsAtMinRatio) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 10.0f, 0.0f, 0.0f };
        l.velocity = { 0.0f, 0.0f, 0.0f };
        // Extreme source velocity ⇒ ratio hits ceiling (2.0) ⇒ factor floor (0.5).
        v.velocity = { -1.0e6f, 0.0f, 0.0f };
        const float f = AudioSpatializer::dopplerFactor(l, v);
        CHECK(f >= 0.5f - 1e-4f);
        CHECK(f <= 2.0f + 1e-4f);
    }

    TEST_CASE(DopplerFloorAtMinDistance) {
        ListenerParams l = defaultListener();
        SpatializationParams v = defaultVoice();
        v.position = { 0.0f, 0.0f, 0.0f };   // co-located with listener
        v.velocity = { 1.0e6f, 0.0f, 0.0f };
        l.velocity = { -1.0e6f, 0.0f, 0.0f };
        const float f = AudioSpatializer::dopplerFactor(l, v);
        // No NaN, no inf, within ratio envelope.
        CHECK(f == f);                          // not NaN
        CHECK(f >= 0.5f - 1e-4f);
        CHECK(f <= 2.0f + 1e-4f);
    }
TEST_SUITE_END
