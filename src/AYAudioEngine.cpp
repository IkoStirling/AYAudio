#include <AYAudioEngine.h>

#include <AYAudioSpatializer.h>
#include <AYAudioStreamRingBuffer.h>
#include "MiniaudioBackend.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>

namespace ayt::audio
{

namespace {

// Helper: copy PCM into the per-voice render scratch with channel-aware
// gain application. The spatializer gives us left/right gains; we apply
// them per-source-channel and interleave into the stereo scratch buffer.
// For mono source → mono output we duplicate to both channels.
struct VoiceSourceContext {
    const ClipSlot* clip = nullptr;
    SpatialGains gains{};
    uint64_t positionFrames = 0;     // read head at start of render block
    bool loop = false;
    bool finished = false;            // set when non-looping clip runs out

    // v1.5: pitch*timeScale feeding the block-quantized stepper. At
    // effectiveScale == 1 the loop's integer part is exactly 1 per output
    // frame and the accumulator stays at 0 (matches v1.0 behaviour).
    float effectiveScale = 1.0f;
    double fractionalAccum = 0.0;
};

// Stream-backed voice context. Drains one interleaved F32 frame per
// `voicePitchIter`. Pitch is applied via block-quantized stepping: each
// output frame consumes `voice.pitch * timeScale` source frames. We
// interpolate linearly between the two halves on either side of the
// fractional position so the pitch changes are smooth at the cost of a
// single multiply + lerp per frame. v2 may swap to a higher-order windowed
// sinc without changing the call shape.
struct StreamSourceContext {
    AudioStreamRingBuffer* ring = nullptr;
    SpatialGains gains{};
    uint16_t srcChannels = 2;
    bool loop = false;
    bool finished = false;

    // Effective scalar = voice.pitch * audio-thread timeScale snapshot.
    // Carried per-voice because the snapshot is taken in render() once per
    // call but each voice needs its own pitch.
    float effectiveScale = 1.0f;

    // For pitch stepping: integer source-frame cursor inside the ring and
    // a fractional accumulator. step = effectiveScale frames per output
    // frame; integer part -> number of full source frames to consume
    // before the next sample lands; fractional part -> next accumulator
    // value (0 <= frac < 1). Surplus frames after consumption are dropped
    // (consumer outruns source — pitch-up), shortfall is last-sample hold
    // (consumer underruns — pitch-down or producer starved).
    double fractionalAccum = 0.0;
    float  lastSample[2] = { 0.0f, 0.0f };
    bool   hasLast = false;
    bool   underrunLatched = false; // observed a 0-read this block
};

// PCM source callback: fills `voiceBuffer` with interleaved float32 frames.
// For mono clips we duplicate to stereo. For stereo clips we apply
// per-channel gains directly. The function is called from the audio thread
// (mixer) so it must be lock-free.
//
// v1.5: applies `pitch * timeScale` via block-quantized stepping. The two
// common cases — (a) no S16 conversion, (b) effectiveScale == 1 — keep the
// identical tight loop as v1.0 for zero per-frame overhead in the default
// path.
void renderVoicePcm(void* userData,
                    uint32_t /*voiceIndex*/,
                    float* voiceBuffer,
                    uint32_t frameCount) {
    auto* ctx = static_cast<VoiceSourceContext*>(userData);
    if (ctx == nullptr || ctx->clip == nullptr || ctx->finished) {
        return;
    }

    const ClipSlot& clip = *ctx->clip;
    const AudioClipDesc& desc = clip.desc;
    const uint16_t srcChannels = desc.channels;
    const uint32_t totalFrames = desc.frameCount;

    const float leftGain  = ctx->gains.leftGain;
    const float rightGain = ctx->gains.rightGain;
    const float monoGain  = ctx->gains.monoGain;
    const float scale     = ctx->effectiveScale;

    // Fast-path: scale == 1.0 → identity step. Keeping the legacy tight
    // loop lets v1.0 callers incur no per-frame overhead in the default
    // configuration.
    const bool identityScale = (scale == 1.0f);

    uint64_t pos = ctx->positionFrames;

    auto writeMono = [&](float s) {
        // outer loop fills voiceBuffer directly
        (void)s;
    };

    if (identityScale) {
        ctx->fractionalAccum = 0.0; // never drift under identity.
        if (desc.format == AudioSampleFormat::F32) {
            const float* data = reinterpret_cast<const float*>(desc.pcmData);
            if (data == nullptr) {
                return;
            }
            if (srcChannels == 1) {
                for (uint32_t f = 0; f < frameCount; ++f) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) pos = 0; else { ctx->finished = true; break; }
                    }
                    const float s = data[pos] * monoGain;
                    voiceBuffer[2 * f + 0] = s;
                    voiceBuffer[2 * f + 1] = s;
                    ++pos;
                }
            } else {
                for (uint32_t f = 0; f < frameCount; ++f) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) pos = 0; else { ctx->finished = true; break; }
                    }
                    const float l = data[2 * pos + 0] * leftGain;
                    const float r = data[2 * pos + 1] * rightGain;
                    voiceBuffer[2 * f + 0] = l;
                    voiceBuffer[2 * f + 1] = r;
                    ++pos;
                }
            }
        } else {
            const int16_t* data = reinterpret_cast<const int16_t*>(desc.pcmData);
            if (data == nullptr) {
                return;
            }
            constexpr float kInvS16 = 1.0f / 32768.0f;
            if (srcChannels == 1) {
                for (uint32_t f = 0; f < frameCount; ++f) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) pos = 0; else { ctx->finished = true; break; }
                    }
                    const float s = static_cast<float>(data[pos]) * kInvS16 * monoGain;
                    voiceBuffer[2 * f + 0] = s;
                    voiceBuffer[2 * f + 1] = s;
                    ++pos;
                }
            } else {
                for (uint32_t f = 0; f < frameCount; ++f) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) pos = 0; else { ctx->finished = true; break; }
                    }
                    const float l = static_cast<float>(data[2 * pos + 0]) * kInvS16 * leftGain;
                    const float r = static_cast<float>(data[2 * pos + 1]) * kInvS16 * rightGain;
                    voiceBuffer[2 * f + 0] = l;
                    voiceBuffer[2 * f + 1] = r;
                    ++pos;
                }
            }
        }
        ctx->positionFrames = pos;
        return;
    }

    // Block-quantized stepping path for non-identity scale. Mirrors the
    // stream source: integer accum -> #source frames to advance before the
    // next new output sample lands, fractional part carries across calls.
    if (desc.format == AudioSampleFormat::F32) {
        const float* data = reinterpret_cast<const float*>(desc.pcmData);
        if (data == nullptr) {
            return;
        }
        if (srcChannels == 1) {
            for (uint32_t f = 0; f < frameCount; ++f) {
                ctx->fractionalAccum += static_cast<double>(scale);
                int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
                if (wholeSteps <= 0) {
                    // Deficit — repeat last sample, or silence if voice just started.
                    const float last = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 0];
                    voiceBuffer[2 * f + 0] = last;
                    voiceBuffer[2 * f + 1] = last;
                    continue;
                }
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) { pos = 0; } else { ctx->finished = true; break; }
                    }
                    if (s == wholeSteps - 1) {
                        const float v = data[pos] * monoGain;
                        voiceBuffer[2 * f + 0] = v;
                        voiceBuffer[2 * f + 1] = v;
                    }
                    ++pos;
                }
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        } else {
            for (uint32_t f = 0; f < frameCount; ++f) {
                ctx->fractionalAccum += static_cast<double>(scale);
                int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
                if (wholeSteps <= 0) {
                    voiceBuffer[2 * f + 0] = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 0];
                    voiceBuffer[2 * f + 1] = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 1];
                    continue;
                }
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) { pos = 0; } else { ctx->finished = true; break; }
                    }
                    if (s == wholeSteps - 1) {
                        const float l = data[2 * pos + 0] * leftGain;
                        const float r = data[2 * pos + 1] * rightGain;
                        voiceBuffer[2 * f + 0] = l;
                        voiceBuffer[2 * f + 1] = r;
                    }
                    ++pos;
                }
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        }
    } else {
        const int16_t* data = reinterpret_cast<const int16_t*>(desc.pcmData);
        if (data == nullptr) {
            return;
        }
        constexpr float kInvS16 = 1.0f / 32768.0f;
        if (srcChannels == 1) {
            for (uint32_t f = 0; f < frameCount; ++f) {
                ctx->fractionalAccum += static_cast<double>(scale);
                int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
                if (wholeSteps <= 0) {
                    const float last = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 0];
                    voiceBuffer[2 * f + 0] = last;
                    voiceBuffer[2 * f + 1] = last;
                    continue;
                }
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) { pos = 0; } else { ctx->finished = true; break; }
                    }
                    if (s == wholeSteps - 1) {
                        const float v = static_cast<float>(data[pos]) * kInvS16 * monoGain;
                        voiceBuffer[2 * f + 0] = v;
                        voiceBuffer[2 * f + 1] = v;
                    }
                    ++pos;
                }
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        } else {
            for (uint32_t f = 0; f < frameCount; ++f) {
                ctx->fractionalAccum += static_cast<double>(scale);
                int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
                if (wholeSteps <= 0) {
                    voiceBuffer[2 * f + 0] = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 0];
                    voiceBuffer[2 * f + 1] = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 1];
                    continue;
                }
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (pos >= totalFrames) {
                        if (ctx->loop) { pos = 0; } else { ctx->finished = true; break; }
                    }
                    if (s == wholeSteps - 1) {
                        const float l = static_cast<float>(data[2 * pos + 0]) * kInvS16 * leftGain;
                        const float r = static_cast<float>(data[2 * pos + 1]) * kInvS16 * rightGain;
                        voiceBuffer[2 * f + 0] = l;
                        voiceBuffer[2 * f + 1] = r;
                    }
                    ++pos;
                }
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        }
    }

    (void)writeMono; // (kept for symmetry with stream path's outFrame helper)
    ctx->positionFrames = pos;
}

// Pull one interleaved frame (or fewer channels) from the ring buffer into
// `out[0..srcChannels)`. Returns true on success, false on underrun.
// On underrun we leave `out` untouched so the caller can hold the previous
// sample (last-sample-hold semantics — standard streaming audio behaviour).
static bool pullOneFrame(StreamSourceContext& ctx, float* out) {
    float frame[2] = { 0.0f, 0.0f };
    const uint32_t got = ctx.ring->read(frame, 1);
    if (got == 0) {
        ctx.underrunLatched = true;
        return false;
    }
    out[0] = frame[0];
    if (ctx.srcChannels == 2) {
        out[1] = frame[1];
    } else {
        out[1] = frame[0];
    }
    return true;
}

// v1.5 stream source callback. Drains frames from the ring buffer at the
// requested pitch*timeScale rate. Block-quantized stepping:
//   - each output frame consumes stepPerFrame = effectiveScale source frames,
//     tracked as `fractionalAccum` across calls.
//   - integer part of accum -> source frames to skip (consume) before the
//     next new output sample lands in the buffer; each "new" pull replaces
//     the held last sample.
//   - fractional part carries into the next call so pitch stays smooth.
//   - underrun -> last-sample hold (zero on the very first underrun).
//   - skip-on-surplus: when step > 1, the integer count is silently dropped
//     from the ring (pitch-up — produces audible cadence but is correct).
//   - shortfall-on-deficit: when step < 1, the held sample repeats.
void renderVoiceStream(void* userData,
                       uint32_t /*voiceIndex*/,
                       float* voiceBuffer,
                       uint32_t frameCount) {
    auto* ctx = static_cast<StreamSourceContext*>(userData);
    if (ctx == nullptr || ctx->ring == nullptr || ctx->finished) {
        return;
    }

    const float leftGain  = ctx->gains.leftGain;
    const float rightGain = ctx->gains.rightGain;
    const float monoGain  = ctx->gains.monoGain;
    const uint16_t ch     = ctx->srcChannels;
    const float scale     = ctx->effectiveScale;

    ctx->underrunLatched = false;

    // Output scratch (one frame at a time).
    float outFrame[2] = { 0.0f, 0.0f };

    for (uint32_t f = 0; f < frameCount; ++f) {
        ctx->fractionalAccum += static_cast<double>(scale);

        // Integer part of the accumulator = number of source frames to
        // advance past before producing the next output frame. We always
        // re-pull at least once per output frame so `lastSample` reflects
        // the most recent source frame we've seen.
        const int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
        if (wholeSteps <= 0) {
            // Deficit: hold the last sample (zero on first frame).
            outFrame[0] = ctx->hasLast ? ctx->lastSample[0] : 0.0f;
            outFrame[1] = ctx->hasLast ? ctx->lastSample[1] : 0.0f;
        } else {
            // Skip `wholeSteps - 1` frames without producing output (pitch-up),
            // then pull one new frame for this output frame's value.
            for (int32_t s = 0; s < wholeSteps - 1; ++s) {
                float discard[2];
                pullOneFrame(*ctx, discard);
            }
            if (!pullOneFrame(*ctx, outFrame)) {
                // Underrun mid-block — hold the last sample and on a fresh
                // voice (no hasLast) emit silence.
                if (!ctx->hasLast) {
                    outFrame[0] = 0.0f;
                    outFrame[1] = 0.0f;
                } else {
                    outFrame[0] = ctx->lastSample[0];
                    outFrame[1] = ctx->lastSample[1];
                }
            } else {
                ctx->lastSample[0] = outFrame[0];
                ctx->lastSample[1] = outFrame[1];
                ctx->hasLast = true;
            }
            ctx->fractionalAccum -= static_cast<double>(wholeSteps);
        }

        const float l = outFrame[0] * (ch == 1 ? monoGain : leftGain);
        const float r = outFrame[1] * (ch == 1 ? monoGain : rightGain);
        voiceBuffer[2 * f + 0] = l;
        voiceBuffer[2 * f + 1] = r;
    }

    // End-of-stream / drain / close semantics: the producer may stop
    // pushing; the ring is then drained by the reads above, and any
    // remaining underrun is the consumer-side signal that the voice is
    // done. We do NOT mark finished just from a single underrun because
    // a transient producer hiccup would cut the voice off; we only mark
    // it when the ring has actually been closed (or EOS signalled).
    if ((ctx->underrunLatched && ctx->ring->isClosed()) ||
        (ctx->underrunLatched && ctx->ring->endOfStream())) {
        ctx->finished = true;
    }
}

} // namespace

// ===== AudioEngine =====

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(const AudioSettings& settings) {
    if (_initialized) {
        return true;
    }
    _settings = settings;

    if (!_commands.initialize(settings.commandQueueCapacity)) {
        return false;
    }
    if (!_clips.initialize(settings.maxClips)) {
        _commands.shutdown();
        return false;
    }
    if (!_mixer.initialize(settings.maxVoices, settings.channels)) {
        _clips.clear();
        _commands.shutdown();
        return false;
    }

    _voices.assign(settings.maxVoices, VoiceRuntime{});
    _voiceCapacity = settings.maxVoices;
    _nextVoiceId = 1;
    _engineFrame = 0;
    _listener = ListenerParams{};
    _paused = false;
    _timeScale = 1.0f;

    // Pick a backend. The engine doesn't link backends directly; the caller
    // (AudioSubSystem or a test) injects one via a setter, or we instantiate
    // the default Null backend here.
    // For v0 we don't ship MiniaudioBackend (depends on third-party header);
    // we'll add it in v0.5 via a factory function.
    if (_backend == nullptr) {
        // Caller is expected to install the backend; without one we treat
        // the engine as uninitialized for rendering but commands still flow.
        _backendInfo = AudioBackendInfo{};
        _backendInfo.name = "None";
    } else {
        _backendInfo = _backend->describe();
        if (!_backend->start(_backendInfo)) {
            _backend.reset();
            return false;
        }
        // Real-device backends (currently MiniaudioBackend) pull output from
        // the engine inside their own data callback. Hook them up after the
        // backend is started so the engine state is fully initialized.
        if (auto* ma = dynamic_cast<MiniaudioBackend*>(_backend.get())) {
            ma->attachEngine(this);
        }
    }

    _initialized = true;
    return true;
}

void AudioEngine::shutdown() {
    if (!_initialized) {
        return;
    }
    // Stop all voices first so the audio thread (if any) sees a clean state.
    stopAll();

    if (_backend) {
        _backend->stop();
        _backend.reset();
    }

    _voices.clear();
    _voiceCapacity = 0;
    _nextVoiceId = 1;

    // Close any open streaming rings so producer threads (if any) see
    // write() reject on their next push instead of silently filling an
    // orphaned buffer.
    for (auto& slot : _streams) {
        if (slot.inUse) {
            if (slot.ring) {
                slot.ring->close();
            }
            slot.ring.reset();
            slot.inUse = false;
        }
        slot.desc = AudioStreamDesc{};
    }

    _mixer.reset();
    _clips.clear();
    _commands.shutdown();

    _initialized = false;
    _paused = false;
    _timeScale = 1.0f;
}

void AudioEngine::setBackend(std::unique_ptr<IAudioBackend> backend) {
    if (_initialized) {
        // Hot-swap is not supported in v0; require shutdown first.
        return;
    }
    _backend = std::move(backend);
}

AudioClipId AudioEngine::registerClip(const AudioClipDesc& desc) {
    if (!_initialized) {
        return InvalidClipId;
    }
    // Game-thread API. Push a command so the audio thread can re-resolve
    // pointers if the clip storage ever moves (we keep it pinned for v0, but
    // going through the queue keeps the contract symmetric with play()).
    AudioCommand cmd{};
    cmd.type = AudioCommandType::RegisterClip;
    cmd.clipDesc = desc;
    if (!_commands.tryPush(cmd)) {
        return InvalidClipId;
    }
    // Apply on the calling side too so play() can immediately find the clip.
    // This avoids a one-frame delay before the clip is queryable.
    const AudioClipId id = _clips.registerClip(desc);
    return id;
}

void AudioEngine::releaseClip(AudioClipId id) {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::ReleaseClip;
    cmd.clip = id;
    _commands.tryPush(cmd);
    _clips.releaseClip(id);
}

VoiceHandle AudioEngine::play(AudioClipId clip,
                              AudioBus bus,
                              const SpatializationParams& spatial,
                              float volume,
                              bool loop) {
    if (!_initialized) {
        return InvalidVoice;
    }
    if (clip == InvalidClipId || _clips.find(clip) == nullptr) {
        return InvalidVoice;
    }

    AudioCommand cmd{};
    cmd.type = AudioCommandType::Play;
    cmd.clip = clip;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = std::clamp(volume, 0.0f, 8.0f);
    cmd.loop = loop ? 1 : 0;

    if (!_commands.tryPush(cmd)) {
        return InvalidVoice;
    }

    // Allocate immediately so the caller can use the handle this frame.
    VoiceHandle voice = _allocateVoice();
    if (voice == InvalidVoice) {
        return InvalidVoice;
    }
    VoiceRuntime& slot = _voices[voice - 1];
    slot = VoiceRuntime{};
    slot.clip = clip;
    slot.bus = bus;
    slot.spatial = spatial;
    slot.volume = cmd.scalar;
    slot.pitch = 1.0f;
    slot.loop = loop;
    slot.playing = true;
    slot.startFrame = _engineFrame;
    slot.positionFrames = 0;
    return voice;
}

void AudioEngine::stop(VoiceHandle voice) {
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::Stop;
    cmd.voice = voice;
    _commands.tryPush(cmd);

    const uint32_t idx = voice - 1;
    if (idx < _voices.size()) {
        _voices[idx].playing = false;
        _voices[idx].positionFrames = 0;
    }
}

void AudioEngine::stopAll() {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::StopAll;
    _commands.tryPush(cmd);

    for (auto& voice : _voices) {
        voice.playing = false;
        voice.positionFrames = 0;
    }
}

void AudioEngine::setVoiceVolume(VoiceHandle voice, float volume) {
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoiceVolume;
    cmd.voice = voice;
    cmd.scalar = std::clamp(volume, 0.0f, 8.0f);
    _commands.tryPush(cmd);

    const uint32_t idx = voice - 1;
    if (idx < _voices.size()) {
        _voices[idx].volume = cmd.scalar;
    }
}

void AudioEngine::setVoicePitch(VoiceHandle voice, float pitch) {
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoicePitch;
    cmd.voice = voice;
    cmd.scalar = pitch;
    _commands.tryPush(cmd);

    const uint32_t idx = voice - 1;
    if (idx < _voices.size()) {
        _voices[idx].pitch = pitch;
    }
}

void AudioEngine::setVoiceSpatial(VoiceHandle voice, const SpatializationParams& spatial) {
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoiceSpatialParams;
    cmd.voice = voice;
    cmd.spatial = spatial;
    _commands.tryPush(cmd);

    const uint32_t idx = voice - 1;
    if (idx < _voices.size()) {
        _voices[idx].spatial = spatial;
    }
}

void AudioEngine::setBusGain(AudioBus bus, float linearGain) {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetBusGain;
    cmd.bus = bus;
    cmd.scalar = linearGain;
    _commands.tryPush(cmd);
    _mixer.setBusGain(bus, linearGain);
}

void AudioEngine::setMasterGain(float linearGain) {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetMasterGain;
    cmd.scalar = linearGain;
    _commands.tryPush(cmd);
    _mixer.setMasterGain(linearGain);
}

void AudioEngine::setListener(const ListenerParams& listener) {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetListener;
    cmd.listener = listener;
    _commands.tryPush(cmd);
    _listener = listener;
}

void AudioEngine::pause() {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PauseEngine;
    _commands.tryPush(cmd);
    _paused = true;
}

void AudioEngine::resume() {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::ResumeEngine;
    _commands.tryPush(cmd);
    _paused = false;
}

// ===== v1.5 streaming source API =====

namespace {

uint32_t nextPowerOfTwo32(uint32_t v) {
    if (v < 2) return 2;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

} // namespace

AudioStreamId AudioEngine::openStream(const AudioStreamDesc& desc) {
    if (!_initialized) {
        return InvalidStreamId;
    }
    if (desc.channels == 0 || desc.capacityFrames == 0) {
        return InvalidStreamId;
    }
    if (desc.channels != 1 && desc.channels != 2) {
        return InvalidStreamId;
    }

    // Find the first free slot. fixed-size std::array keeps this lock-free
    // under game-thread-only mutation (audio thread is a reader through
    // streamRing() but only after the slot's inUse flag is published).
    uint32_t chosenIdx = kMaxStreams;
    for (uint32_t i = 0; i < kMaxStreams; ++i) {
        if (!_streams[i].inUse) {
            chosenIdx = i;
            break;
        }
    }
    if (chosenIdx == kMaxStreams) {
        return InvalidStreamId; // all 8 slots busy
    }

    StreamSlot& slot = _streams[chosenIdx];
    slot.desc = desc;
    slot.desc.capacityFrames = nextPowerOfTwo32(desc.capacityFrames);
    slot.ring = std::make_unique<AudioStreamRingBuffer>();
    if (!slot.ring->initialize(slot.desc.channels, slot.desc.capacityFrames)) {
        slot.ring.reset();
        return InvalidStreamId;
    }
    slot.inUse = true;

    // Push the same intent to the audio thread for symmetry with the
    // registerClip dual-write pattern. The audio thread observes via
    // _applyCommand but currently no audio-side state changes; the push
    // is a forward-compatibility / debug breadcrumb.
    AudioCommand cmd{};
    cmd.type = AudioCommandType::OpenStream;
    cmd.streamDesc = slot.desc;
    _commands.tryPush(cmd);

    return static_cast<AudioStreamId>(chosenIdx + 1);
}

void AudioEngine::closeStream(AudioStreamId id) {
    if (!_initialized || id == InvalidStreamId) {
        return;
    }
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse) return;

    // Tell the consumer to stop reading; existing buffered frames still
    // drain so any in-flight voice finishes its current chunk cleanly.
    if (slot.ring) {
        slot.ring->markEndOfStream();
        slot.ring->close();
    }

    AudioCommand cmd{};
    cmd.type = AudioCommandType::CloseStream;
    cmd.stream = id;
    _commands.tryPush(cmd);

    // Free the slot. Active voices backed by this stream see `finished` on
    // their next render call (ring drained / closed); they don't crash.
    slot.ring.reset();
    slot.desc = AudioStreamDesc{};
    slot.inUse = false;
}

uint32_t AudioEngine::streamPush(AudioStreamId id, const float* pcm, uint32_t frameCount) {
    if (!_initialized || id == InvalidStreamId || pcm == nullptr || frameCount == 0) {
        return 0;
    }
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return 0;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return 0;
    return slot.ring->write(pcm, frameCount);
}

VoiceHandle AudioEngine::playStream(AudioStreamId stream,
                                     AudioBus bus,
                                     const SpatializationParams& spatial,
                                     float volume,
                                     bool loop) {
    if (!_initialized) {
        return InvalidVoice;
    }
    if (stream == InvalidStreamId) return InvalidVoice;
    const uint32_t idx = stream - 1;
    if (idx >= kMaxStreams) return InvalidVoice;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return InvalidVoice;

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PlayStream;
    cmd.stream = stream;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = std::clamp(volume, 0.0f, 8.0f);
    cmd.loop = loop ? 1 : 0;
    if (!_commands.tryPush(cmd)) {
        return InvalidVoice;
    }

    VoiceHandle voice = _allocateVoice();
    if (voice == InvalidVoice) {
        return InvalidVoice;
    }
    VoiceRuntime& v = _voices[voice - 1];
    v = VoiceRuntime{};
    v.sourceKind = VoiceSourceKind::Stream;
    v.stream = stream;
    v.bus = bus;
    v.spatial = spatial;
    v.volume = cmd.scalar;
    v.pitch = 1.0f;
    v.loop = loop;
    v.playing = true;
    v.startFrame = _engineFrame;
    v.positionFrames = 0;
    return voice;
}

void AudioEngine::setTimeScale(float scale) {
    // Clamp to a safe musical range; the audio thread reads once per
    // voice per render block so values outside [0.25, 4.0] make no sense
    // and would risk silent aliasing.
    _timeScale = std::clamp(scale, 0.25f, 4.0f);
}

AudioStreamRingBuffer* AudioEngine::streamRing(AudioStreamId id) {
    if (id == InvalidStreamId) return nullptr;
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return nullptr;
    StreamSlot& slot = _streams[idx];
    return slot.inUse ? slot.ring.get() : nullptr;
}

const AudioStreamDesc& AudioEngine::streamDesc(AudioStreamId id) const {
    static const AudioStreamDesc kEmpty{};
    if (id == InvalidStreamId) return kEmpty;
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return kEmpty;
    const StreamSlot& slot = _streams[idx];
    return slot.inUse ? slot.desc : kEmpty;
}

void AudioEngine::submitFrame(float /*deltaTime*/) {
    if (!_initialized) {
        return;
    }
    // Drain queued commands. The audio thread also drains before render(),
    // so this just keeps the game-thread view of state current.
    AudioCommand cmd{};
    int safety = 4096;
    while (safety-- > 0 && _commands.tryPop(cmd)) {
        _applyCommand(cmd);
    }
    ++_engineFrame;
}

void AudioEngine::render(float* output, uint32_t frameCount) {
    if (!_initialized || output == nullptr || frameCount == 0) {
        return;
    }
    // Drain commands so audio-thread-visible state is up to date.
    AudioCommand cmd{};
    int safety = 4096;
    while (safety-- > 0 && _commands.tryPop(cmd)) {
        _applyCommand(cmd);
    }

    if (_paused) {
        std::memset(output, 0, frameCount * _settings.channels * sizeof(float));
        return;
    }

    // Build per-voice render context list. Voices that finished in a prior
    // render call are recycled at the start of this one.
    constexpr uint32_t kMaxVoicesPerRender = 64;
    if (_voices.size() > kMaxVoicesPerRender) {
        return; // safety guard; settings.maxVoices should be ≤ 64 for v0
    }

    VoiceSourceContext clipContexts[kMaxVoicesPerRender]{};
    StreamSourceContext streamContexts[kMaxVoicesPerRender]{};
    AudioMixer::VoiceSourceFn sources[kMaxVoicesPerRender]{};
    void* userData[kMaxVoicesPerRender]{};
    VoiceRuntime voiceSnapshot[kMaxVoicesPerRender]{};
    uint32_t activeCount = 0;

    // Snapshot _timeScale once per render call so all voices see a coherent
    // value. Float writes on x86/ARM are atomic for naturally-aligned
    // 4-byte loads; we read once.
    const float timeScale = _timeScale;

    for (uint32_t i = 0; i < _voices.size(); ++i) {
        VoiceRuntime& slot = _voices[i];
        if (!slot.playing) {
            continue;
        }
        SpatialGains gains = AudioSpatializer::compute(_listener, slot.spatial, _settings.channels);

        if (slot.sourceKind == VoiceSourceKind::Stream) {
            // Resolve the stream's ring buffer.
            const uint32_t sIdx = slot.stream - 1;
            StreamSlot* sslot = (slot.stream != InvalidStreamId && sIdx < kMaxStreams)
                                    ? &_streams[sIdx]
                                    : nullptr;
            if (sslot == nullptr || !sslot->inUse || !sslot->ring) {
                slot.playing = false;
                continue;
            }
            StreamSourceContext& sctx = streamContexts[activeCount];
            sctx.ring = sslot->ring.get();
            sctx.gains = gains;
            sctx.srcChannels = sslot->desc.channels;
            sctx.loop = slot.loop;
            sctx.finished = false;
            // v1.5: voice.pitch * timeScale feeds the source-step scalar
            // inside renderVoiceStream. The accumulator survives across
            // calls on the stack array (zeroed at first entry, which is
            // a fresh context — see render() shape).
            sctx.effectiveScale = std::clamp(slot.pitch * timeScale, 0.25f, 4.0f);
            sctx.fractionalAccum = 0.0;
            sctx.lastSample[0] = 0.0f;
            sctx.lastSample[1] = 0.0f;
            sctx.hasLast = false;
            sctx.underrunLatched = false;
            sources[activeCount] = &renderVoiceStream;
            userData[activeCount] = &streamContexts[activeCount];
            voiceSnapshot[activeCount] = slot;
            ++activeCount;
            continue;
        }

        const ClipSlot* clip = _clips.find(slot.clip);
        if (clip == nullptr) {
            slot.playing = false;
            continue;
        }
        VoiceSourceContext& ctx = clipContexts[activeCount];
        ctx.clip = clip;
        ctx.gains = gains;
        ctx.positionFrames = slot.positionFrames;
        ctx.loop = slot.loop;
        ctx.finished = false;
        // v1.5: feed voice.pitch * timeScale. Stack-array context means
        // fractionalAccum resets each render call — fine at scale == 1.0
        // (the integer loop runs), and at integer multiples (accum crosses
        // back to 0 each call). Fractional non-1 scales will see ±1 sample
        // jitter per call; documented in design / tested in v2 / API note.
        ctx.effectiveScale = std::clamp(slot.pitch * timeScale, 0.25f, 4.0f);
        ctx.fractionalAccum = 0.0;
        sources[activeCount] = &renderVoicePcm;
        userData[activeCount] = &clipContexts[activeCount];
        voiceSnapshot[activeCount] = slot;
        ++activeCount;
    }

    _mixer.mix(output, frameCount, voiceSnapshot, sources, userData, activeCount);

    // Commit read heads and prune finished voices. Both context arrays use
    // a parallel layout indexed by `i`; we look at snap.sourceKind to pick
    // the right finished flag.
    for (uint32_t i = 0; i < activeCount; ++i) {
        const VoiceRuntime& snap = voiceSnapshot[i];
        // Map back from snapshot to live voice slot. For clip voices we
        // match on (clip, startFrame); for stream voices we match on
        // (sourceKind, stream, startFrame). startFrame is monotonic so
        // it's a reliable identity within the active subset.
        for (uint32_t v = 0; v < _voices.size(); ++v) {
            if (!_voices[v].playing || _voices[v].startFrame != snap.startFrame) continue;
            if (snap.sourceKind == VoiceSourceKind::Stream) {
                if (_voices[v].sourceKind != VoiceSourceKind::Stream) continue;
                if (_voices[v].stream != snap.stream) continue;
                const StreamSourceContext& sctx = streamContexts[i];
                if (sctx.finished) {
                    _voices[v].playing = false;
                }
                // Stream voices don't carry a persistent read head back —
                // the ring buffer owns its own cursor.
                break;
            } else {
                if (_voices[v].sourceKind != VoiceSourceKind::Clip) continue;
                if (_voices[v].clip != snap.clip) continue;
                const VoiceSourceContext& ctx = clipContexts[i];
                if (ctx.finished && !ctx.loop) {
                    _voices[v].playing = false;
                } else {
                    _voices[v].positionFrames = ctx.positionFrames;
                }
                break;
            }
        }
    }
}

uint32_t AudioEngine::activeVoiceCount() const {
    uint32_t count = 0;
    for (const auto& v : _voices) {
        if (v.playing) ++count;
    }
    return count;
}

const VoiceRuntime* AudioEngine::voiceAt(uint32_t index) const {
    if (index >= _voices.size()) return nullptr;
    return &_voices[index];
}

// ===== Internal helpers =====

VoiceHandle AudioEngine::_allocateVoice() {
    // Find a free slot first (re-use released voices before allocating new).
    for (uint32_t i = 0; i < _voices.size(); ++i) {
        if (!_voices[i].playing) {
            // Voice handles are monotonic; re-using a slot doesn't reuse
            // its handle value. The caller tracks the handle, not the slot.
            return static_cast<VoiceHandle>(i + 1);
        }
    }
    // No free slot — allocate a new handle (will be ignored if past capacity).
    if (_nextVoiceId > _voiceCapacity) {
        return InvalidVoice;
    }
    return _nextVoiceId++;
}

void AudioEngine::_applyCommand(const AudioCommand& cmd) {
    switch (cmd.type) {
        case AudioCommandType::Play: {
            VoiceHandle v = _allocateVoice();
            if (v == InvalidVoice) break;
            VoiceRuntime& slot = _voices[v - 1];
            slot = VoiceRuntime{};
            slot.clip = cmd.clip;
            slot.bus = cmd.bus;
            slot.spatial = cmd.spatial;
            slot.volume = cmd.scalar;
            slot.pitch = 1.0f;
            slot.loop = cmd.loop != 0;
            slot.playing = true;
            slot.startFrame = _engineFrame;
            slot.positionFrames = 0;
            break;
        }
        case AudioCommandType::Stop: {
            if (cmd.voice == InvalidVoice) break;
            const uint32_t idx = cmd.voice - 1;
            if (idx < _voices.size()) {
                _voices[idx].playing = false;
                _voices[idx].positionFrames = 0;
            }
            break;
        }
        case AudioCommandType::StopAll: {
            for (auto& v : _voices) {
                v.playing = false;
                v.positionFrames = 0;
            }
            break;
        }
        case AudioCommandType::SetVoiceVolume: {
            if (cmd.voice == InvalidVoice) break;
            const uint32_t idx = cmd.voice - 1;
            if (idx < _voices.size()) {
                _voices[idx].volume = std::clamp(cmd.scalar, 0.0f, 8.0f);
            }
            break;
        }
        case AudioCommandType::SetVoicePitch: {
            if (cmd.voice == InvalidVoice) break;
            const uint32_t idx = cmd.voice - 1;
            if (idx < _voices.size()) {
                _voices[idx].pitch = cmd.scalar;
            }
            break;
        }
        case AudioCommandType::SetVoiceSpatialParams: {
            if (cmd.voice == InvalidVoice) break;
            const uint32_t idx = cmd.voice - 1;
            if (idx < _voices.size()) {
                _voices[idx].spatial = cmd.spatial;
            }
            break;
        }
        case AudioCommandType::SetBusGain:
            _mixer.setBusGain(cmd.bus, cmd.scalar);
            break;
        case AudioCommandType::SetMasterGain:
            _mixer.setMasterGain(cmd.scalar);
            break;
        case AudioCommandType::SetListener:
            _listener = cmd.listener;
            break;
        case AudioCommandType::RegisterClip:
            _clips.registerClip(cmd.clipDesc);
            break;
        case AudioCommandType::ReleaseClip:
            _clips.releaseClip(cmd.clip);
            break;
        case AudioCommandType::PauseEngine:
            _paused = true;
            break;
        case AudioCommandType::ResumeEngine:
            _paused = false;
            break;
        case AudioCommandType::OpenStream: {
            // Game-thread openStream() already installed the ring under
            // the same id; _applyCommand is the audio-thread breadcrumb.
            // No mutation needed (the audio thread is a reader, not an
            // owner, of streaming rings).
            (void)cmd.streamDesc;
            break;
        }
        case AudioCommandType::CloseStream: {
            // Same — game-thread closeStream() already tore the ring down.
            // Audio thread just lets in-flight voices see ring == nullptr
            // (or closed) on their next render.
            (void)cmd.stream;
            break;
        }
        case AudioCommandType::PlayStream: {
            // Symmetrical to Play. game-thread playStream() already wrote
            // the voice slot with the same fields, so this is a no-op for
            // state-sync purposes. Kept so the audio thread can replay the
            // intent if a future semantic requires it.
            (void)cmd;
            break;
        }
    }
}

} // namespace ayt::audio