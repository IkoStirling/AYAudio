#include <AYAudioSpatializer.h>

#include <algorithm>
#include <cmath>

namespace ayt::audio
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float safeSqrt(float v) {
    return v <= 0.0f ? 0.0f : std::sqrt(v);
}

} // namespace

void AudioSpatializer::equalPowerPan(float pan, float& leftGain, float& rightGain) {
    // Map pan [-1, +1] to angle [-π/4, +π/4] for equal-power stereo:
    //   L = cos(θ + π/4), R = sin(θ + π/4), where θ ∈ [-π/4, +π/4].
    // At pan=0 we get L=R≈0.707 (-3 dB center).
    float clamped = clampf(pan, -1.0f, 1.0f);
    float angle = clamped * kHalfPi * 0.5f + kHalfPi * 0.5f;
    leftGain  = std::cos(angle);
    rightGain = std::sin(angle);
}

float AudioSpatializer::distanceGain(const SpatializationParams& voice, float distance) {
    using Model = SpatializationParams::AttenuationModel;
    float ref = std::max(0.0001f, voice.referenceDistance);
    float max = std::max(ref, voice.maxDistance);

    float gain = 1.0f;
    switch (voice.attenuation) {
    case Model::Inverse: {
        // 1 / (1 + rolloff * (d - ref)), no clamp at max.
        float d = std::max(distance, ref);
        gain = 1.0f / (1.0f + voice.rolloffFactor * (d - ref));
        break;
    }
    case Model::Linear: {
        // 1 - (d - ref) / (max - ref), clamped to [0, 1].
        float denom = std::max(0.0001f, max - ref);
        gain = 1.0f - clampf((distance - ref) / denom, 0.0f, 1.0f);
        break;
    }
    case Model::InverseClamped: {
        float d = clampf(distance, ref, max);
        gain = ref / std::max(0.0001f, d);
        gain *= voice.rolloffFactor;
        break;
    }
    case Model::CustomCurve:
    default:
        // v2 — fall through to inverse clamped as the safe default.
        gain = ref / std::max(0.0001f, clampf(distance, ref, max));
        break;
    }
    return clampf(gain, voice.minGain, voice.maxGain);
}

ayt::math::FVector3 AudioSpatializer::toListenerLocal(const ListenerParams& listener,
                                                     const ayt::math::FVector3& world) {
    // Translate into listener space (no rotation): the full rotation matrix
    // (yaw/pitch from forward + up) is folded into azimuth() so we can keep
    // the spatializer math in 2D for v0.
    ayt::math::FVector3 rel = world - listener.position;
    rel *= listener.masterSpatialScale;
    return rel;
}

float AudioSpatializer::worldDistance(const ListenerParams& listener,
                                      const ayt::math::FVector3& a,
                                      const ayt::math::FVector3& b) {
    float dx = (a.x - b.x) * listener.masterSpatialScale;
    float dy = (a.y - b.y) * listener.masterSpatialScale;
    float dz = (a.z - b.z) * listener.masterSpatialScale;
    return safeSqrt(dx * dx + dy * dy + dz * dz);
}

float AudioSpatializer::azimuth(const ListenerParams& listener,
                                const SpatializationParams& voice) {
    ayt::math::FVector3 rel = toListenerLocal(listener, voice.position);
    // Listener forward defaults to -Z; we project the relative vector onto
    // the XZ plane to get a 2D azimuth in radians.
    //   forward = (0, 0, -1)  →  azimuth 0 directly ahead, +π/2 to the right
    float forwardX = listener.forward.x;
    float forwardZ = listener.forward.z;
    float forwardLen = safeSqrt(forwardX * forwardX + forwardZ * forwardZ);
    if (forwardLen <= 0.0001f) {
        // Degenerate forward; treat world +Z as "front".
        forwardX = 0.0f;
        forwardZ = 1.0f;
        forwardLen = 1.0f;
    }
    forwardX /= forwardLen;
    forwardZ /= forwardLen;

    // Dot and cross (XZ) for angle. We want a source at +X (when listener
    // looks down -Z) to give a positive azimuth (right). Standard atan2(cross,
    // dot) yields a negative azimuth for that geometry, so we negate cross.
    float dot = rel.x * forwardX + rel.z * forwardZ;
    float cross = rel.z * forwardX - rel.x * forwardZ;
    return std::atan2(cross, dot);
}

SpatialGains AudioSpatializer::compute(const ListenerParams& listener,
                                       const SpatializationParams& voice,
                                       uint16_t outputChannels) {
    SpatialGains gains{};
    if (outputChannels == 0) {
        return gains;
    }

    using Mode = SpatializationParams::Mode;

    if (voice.mode == Mode::Disabled || voice.spatialBlend <= 0.0f) {
        // 2D path: equal-power pan only.
        float pan = clampf(voice.stereoPan, -1.0f, 1.0f);
        float l = 0.0f, r = 0.0f;
        equalPowerPan(pan, l, r);
        gains.leftGain  = l;
        gains.rightGain = r;
        gains.monoGain  = 0.5f * (l + r);
        return gains;
    }

    // World / Relative — blend 3D attenuation with 2D pan.
    float distance = worldDistance(listener, ayt::math::FVector3{}, voice.position);
    float att = distanceGain(voice, distance);

    // Pan from azimuth.
    float az = azimuth(listener, voice);
    float pan = std::sin(az);   // sin maps -π/2..+π/2 → -1..+1, monotonic

    float l3 = 0.0f, r3 = 0.0f;
    equalPowerPan(pan, l3, r3);
    l3 *= att;
    r3 *= att;

    // Blend with the user's 2D stereoPan when spatialBlend < 1.
    float l2 = 0.0f, r2 = 0.0f;
    equalPowerPan(voice.stereoPan, l2, r2);

    float blend = clampf(voice.spatialBlend, 0.0f, 1.0f);
    gains.leftGain  = l2 * (1.0f - blend) + l3 * blend;
    gains.rightGain = r2 * (1.0f - blend) + r3 * blend;
    gains.monoGain  = att;       // mono bus already collapses the stereo pair
    return gains;
}

} // namespace ayt::audio