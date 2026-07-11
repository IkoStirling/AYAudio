#pragma once
// AYAudioSpatializer.h - Per-voice 3D / pan gain computation
//
// Pure functions (no state) that compute the spatial gain scalars for one
// voice. Returned values are applied by the mixer when summing into the
// output buffer.
//
// v0 implements:
//   - Mode::Disabled   → pure stereoPan with equal-power law
//   - Mode::World      → distance attenuation + stereo pan from azimuth
//
// Attenuation models: Inverse / Linear / InverseClamped.
// Cone gain, occlusion low-pass, and air absorption are wired up in v1
// (see design §5.3); parameters are accepted now to keep call sites stable.

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
};

} // namespace ayt::audio