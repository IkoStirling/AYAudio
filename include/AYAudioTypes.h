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
using AudioStreamId = uint32_t;

constexpr AudioClipId InvalidClipId = 0;
constexpr VoiceHandle InvalidVoice = 0;
constexpr AudioStreamId InvalidStreamId = 0;

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
        CustomCurve,     // v2.0b: sample authored curve
    };

    Mode mode = Mode::Disabled;
    AttenuationModel attenuation = AttenuationModel::InverseClamped;

    ayt::math::FVector3 position{};
    ayt::math::FVector3 velocity{};        // doppler (v2.0b)
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

    float reverbSend                = 0.0f; // 0..1 to reverb bus (v2.0b)
    uint8_t environmentPreset       = 0;    // small room / hall (v2.0b)

    float sourceRadius = 0.0f;
    uint8_t priority   = 128;

    // v2.0b: custom attenuation curve (authored control points).
    // Active when `attenuation == AttenuationModel::CustomCurve`.
    // Distance is normalized into [referenceDistance, maxDistance] → [0, 1]
    // then linearly interpolated between two adjacent samples.
    // If `customCurveCount == 0` the spatializer falls back to
    // InverseClamped so default-constructed voices still produce sound.
    struct CustomAttenuationPoint {
        float distance = 0.0f;   // meters; samples should be ascending
        float gain     = 1.0f;   // linear [0..1]
    };
    static constexpr uint8_t kMaxCustomCurvePoints = 8;
    CustomAttenuationPoint customCurve[kMaxCustomCurvePoints]{};
    uint8_t customCurveCount = 0;
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

    // v2.0b: reverb (master wet return) + ducking (Music bus sidechain).
    // Defaults match engine defaults; AudioEngine copies these into the
    // mixer at initialize() time. Setting depthDb == 0 disables ducking.
    float reverbReturn       = 0.25f;      // master wet scale [0..1]
    float duckDepthDb        = 0.0f;       // ≤ 0 dB; 0 = inactive
    float duckAttackPerSec   = 8.0f;       // upward ramp rate (dB/s)
    float duckReleasePerSec  = 2.0f;       // downward ramp rate (dB/s)
};

// ===== Per-voice runtime state (CPU side) =====

// v1.5: streaming source descriptor. The producer (Resource / loader) calls
// AudioEngine::openStream(desc) once, then asynchronously pushes interleaved
// F32 PCM frames via streamPush(). Frame count is per-channel frames in the
// stored interleaved layout.
struct AudioStreamDesc {
    uint16_t channels = 2;            // 1=mono, 2=stereo
    uint32_t sampleRate = 48000;      // informational; engine doesn't resample
    uint32_t capacityFrames = 16384;  // ring buffer capacity (per-channel
                                      // frames). Rounded up to power of two.
                                      // Default ~340ms @ 48kHz stereo.
    bool loop = false;                // voice-stops-at-drain semantics even
                                      // if loop=true; producer owns re-push.
};

// ===== Per-voice runtime state (CPU side) =====

enum class VoiceSourceKind : uint8_t {
    Clip,       // backed by a registered AudioClipDesc
    Stream,     // backed by an AudioStreamRingBuffer (v1.5)
};

struct VoiceRuntime {
    AudioClipId clip = InvalidClipId;
    AudioBus bus = AudioBus::Sfx;
    SpatializationParams spatial{};

    float volume = 1.0f;        // user-set voice volume
    float pitch  = 1.0f;        // playback rate scalar (consumed in v1.5)
    bool  loop   = false;
    bool  playing = false;

    uint64_t startFrame = 0;    // engine frame when started
    uint64_t positionFrames = 0;// current read head

    // Monotonic identity assigned at play() / playStream() time. Lets the
    // audio thread match a queued Play command back to the slot the game
    // thread already populated, so we don't dual-allocate (see
    // AudioEngine::_applyCommand). 0 = unallocated. Survives across
    // stop()/play() cycles because the counter is engine-wide.
    uint64_t voiceId = 0;

    // v1.5: dispatch source. When sourceKind == Stream, `stream` is the
    // AudioStreamId of the ring buffer feeding the voice. `clip` is unused.
    VoiceSourceKind sourceKind = VoiceSourceKind::Clip;
    AudioStreamId   stream     = InvalidStreamId;
};

} // namespace ayt::audio