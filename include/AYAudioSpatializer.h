#pragma once
// AYAudioSpatializer.h - Per-voice 3D / pan gain computation
//
// Pure functions (no state) that compute the spatial gain scalars for one
// voice. Returned values are applied by the mixer when summing into the
// output buffer.
//
// v1 implements:
//   - Mode::Disabled   → pure stereoPan with equal-power law
//   - Mode::World      → distance + cone + occlusion/obstruction gain
//                          + stereo pan from azimuth
//   - Mode::Relative   → same as World but position is interpreted as an
//                          offset from listener (UI attach)
//
// Attenuation models: Inverse / Linear / InverseClamped.
// Cone gain and occlusion/obstruction gain are wired up in v1.
// Low-pass filtering (occlusion cutoff, air absorption) is v2.

#include <AYCore.h>
#include <AYAudioTypes.h>
#include <cstdint>

namespace ayt::audio
{

struct SpatialGains {
    float leftGain = 1.0f;
    float rightGain = 1.0f;
    float monoGain = 1.0f;       // populated only when output is mono
};

class AudioSpatializer {
public:
    // Compute the per-channel gains for one voice.
    // - listener: pose + scale
    // - voice:    spatial params for this voice
    // - outputChannels: 1 or 2
    // The function is pure: same inputs ⇒ same outputs, no side effects.
    static SpatialGains compute(const ListenerParams& listener,
                                const SpatializationParams& voice,
                                uint16_t outputChannels);

    // Apply equal-power pan law to a stereo pair. pan ∈ [-1, +1].
    static void equalPowerPan(float pan, float& leftGain, float& rightGain);

    // Distance attenuation in linear gain ∈ [minGain, maxGain].
    // distance is in the same units as refDist/maxDist (typically meters).
    static float distanceGain(const SpatializationParams& voice, float distance);

    // Cone attenuation gain ∈ [outerConeGain, 1.0] based on the angle between
    // the listener-relative direction and `voice.forward`. An inner angle of
    // 360° (default) is omnidirectional and returns 1.0 regardless of angle.
    // static暴露用于测试 + 让编辑器和调试工具单独查询 cone gain。
    static float coneGain(const ListenerParams& listener,
                          const SpatializationParams& voice);

    // Occlusion / obstruction gain multiplier ∈ [0, 1]. Combines the two
    // sources multiplicatively: clear + clear ⇒ 1.0; fully occluded ⇒ 0.
    //   gain = (1 - occlusion) * (1 - obstruction * 0.5)
    // Obstruction is treated as a softer attenuation than occlusion. Both
    // inputs are clamped to [0, 1].
    static float occlusionGain(const SpatializationParams& voice);

    // Compute azimuth (radians) of `voice.position` relative to listener
    // forward axis (in listener-local frame). Returns 0 when the source is
    // directly in front, +π/2 to the right, -π/2 to the left.
    static float azimuth(const ListenerParams& listener,
                         const SpatializationParams& voice);

    // Helper: convert a world position into listener-local space.
    static ayt::math::FVector3 toListenerLocal(const ListenerParams& listener,
                                               const ayt::math::FVector3& world);

    // Distance between two world points after applying masterSpatialScale.
    static float worldDistance(const ListenerParams& listener,
                               const ayt::math::FVector3& a,
                               const ayt::math::FVector3& b);

    // v2.0b: doppler pitch factor for a (listener, voice) pair. Returns
    // a scalar in [0.5, 2.0] (the inverse of the doppler ratio). 1.0 means
    // no shift. Pure function — game thread or audio thread safe.
    static float dopplerFactor(const ListenerParams& listener,
                               const SpatializationParams& voice);
};

} // namespace ayt::audio