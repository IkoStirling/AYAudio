#pragma once
// AYAudioTypes.h - Shared audio value types
//
// These types are used by both the engine and the backends. They are pure
// POD/aggregate types with no engine dependencies other than <cstdint> and
// the math vector type, so they are safe to include from any module.

#include <AYCore.h>
#include <AYMath.h>
#include <cstdint>

namespace ayt::audio
{

// ===== Handles =====
// 0 is reserved as the invalid handle, matching the engine convention.

using AudioClipId = uint32_t;
using VoiceHandle = uint32_t;

constexpr AudioClipId InvalidClipId = 0;
constexpr VoiceHandle InvalidVoice = 0;

// ===== Bus taxonomy =====

enum class AudioBus : uint8_t {
    Master = 0,
    Music,
    Sfx,
    Ui,
    Voice,
    Count,
};

// ===== PCM descriptor (game thread → engine) =====

enum class AudioSampleFormat : uint8_t {
    S16,    // interleaved signed 16-bit
    F32,    // interleaved float32
};

struct AudioClipDesc {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;          // 1=mono, 2=stereo (interleaved)
    uint16_t bitsPerSample = 32;    // 16 (S16) or 32 (F32). Mirrors IAudio's
                                    // bitsPerSample for debug/inspectability;
                                    // the format enum below is what the
                                    // engine dispatch actually reads.
    AudioSampleFormat format = AudioSampleFormat::F32;
    uint32_t frameCount = 0;        // samples per channel
    const void* pcmData = nullptr;  // owned by caller until releaseClip()
};

// ===== 3D spatialization parameters =====
//
// v0 implements a subset: mode (2D / World), attenuation (linear/inverse),
// reference/max distances, stereoPan, and spatialBlend. The remaining
// fields are stored but only consumed from v1 onward (see design §11).

struct SpatializationParams {
    enum class Mode : uint8_t {
        Disabled,    // 2D: use StereoPan only
        World,       // 3D world space (default)
        Relative,    // offset from listener (UI attach) — v1
    };

    enum class AttenuationModel : uint8_t {
        Inverse,         // 1 / (1 + rolloff * (d - ref))
        Linear,          // clamp(1 - (d - ref) / (max - ref), 0, 1)
        InverseClamped,  // inverse with clamp at max distance
        CustomCurve,     // v2
    };

    Mode mode = Mode::Disabled;
    AttenuationModel attenuation = AttenuationModel::InverseClamped;

    ayt::math::FVector3 position{};
    ayt::math::FVector3 velocity{};        // doppler (v2)
    ayt::math::FVector3 forward{0, 0, -1};

    float referenceDistance = 1.0f;
    float maxDistance       = 100.0f;
    float rolloffFactor     = 1.0f;
    float minGain           = 0.0f;
    float maxGain           = 1.0f;

    float stereoPan         = 0.0f;        // -1..1 when Mode::Disabled or blend
    float spatialBlend      = 1.0f;        // 0=2D pan only, 1=full 3D

    float innerConeAngleDeg = 360.0f;
    float outerConeAngleDeg = 360.0f;
    float outerConeGain     = 0.0f;

    float airAbsorptionGainPerMeter = 0.0f;
    float occlusion                 = 0.0f;
    float obstruction               = 0.0f;
    float occlusionLowPassHz        = 800.0f;

    float reverbSend                = 0.0f;
    uint8_t environmentPreset       = 0;

    float sourceRadius = 0.0f;
    uint8_t priority   = 128;
};

// ===== Listener =====

struct ListenerParams {
    ayt::math::FVector3 position{};
    ayt::math::FVector3 forward{0, 0, -1};
    ayt::math::FVector3 up{0, 1, 0};
    ayt::math::FVector3 velocity{};

    float masterSpatialScale = 1.0f;       // 1 unit = 1 meter default
    bool hrtfEnabled = false;
};

// ===== Engine settings =====

enum class AudioBackendKind : uint8_t {
    Null,        // no device, drain commands to a sink (test or -no-audio)
    Mock,        // drives unit tests, captures rendered frames for asserts
    Miniaudio,   // real device (v0.5+)
};

struct AudioSettings {
    AudioBackendKind backend = AudioBackendKind::Null;
    uint32_t sampleRate = 48000;
    uint16_t channels   = 2;
    AudioSampleFormat format = AudioSampleFormat::F32;

    uint32_t commandQueueCapacity = 256;   // SPSC ring buffer size (power of 2)
    uint32_t maxVoices = 32;               // active voice cap
    uint32_t maxClips  = 64;               // registered clip cap

    float masterGain = 1.0f;
    bool  enableSpatialization = true;     // master toggle for 3D paths
};

// ===== Per-voice runtime state (CPU side) =====

struct VoiceRuntime {
    AudioClipId clip = InvalidClipId;
    AudioBus bus = AudioBus::Sfx;
    SpatializationParams spatial{};

    float volume = 1.0f;        // user-set voice volume
    float pitch  = 1.0f;        // playback rate scalar (v1)
    bool  loop   = false;
    bool  playing = false;

    uint64_t startFrame = 0;    // engine frame when started
    uint64_t positionFrames = 0;// current read head
};

} // namespace ayt::audio