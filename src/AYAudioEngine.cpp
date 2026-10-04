#include <AYAudio/AudioEngine.h>

#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioSpatializer.h>
#include <AYAudio/AudioStreamRingBuffer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cmath>
#include <vector>

namespace ayt::audio
{

namespace {
std::atomic<uint64_t> gAudioLifecycle{0};

// Helper: copy PCM into the per-voice render scratch with channel-aware
// gain application. The spatializer gives us left/right gains; we apply
// them per-source-channel and interleave into the stereo scratch buffer.
// For mono source → mono output we duplicate to both channels.
struct VoiceSourceContext {
    const ClipSlot* clip = nullptr;
    uint64_t begin = 0, end = 0, fadeOut = 0;
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

    // Cumulative source frames successfully drained from the ring since
    // playStream(). Written back to VoiceRuntime::positionFrames after
    // each render block so game-thread sync clocks (AYVideo audio-master)
    // can query media time via voicePositionFrames().
    uint64_t positionFrames = 0;

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

// PCM source callback uses the guarded registry, interpolates fractional
// source frames, duplicates mono to stereo, and bounds playback to its interval.
void renderVoicePcm(void* userData, uint32_t, float* buffer, uint32_t frames) {
    auto& ctx = *static_cast<VoiceSourceContext*>(userData);
    if (!ctx.clip || ctx.finished) return;
    const auto& d = ctx.clip->desc;
    const uint64_t end = ctx.end ? std::min<uint64_t>(ctx.end, d.frameCount) : d.frameCount;
    if (!d.pcmData || end <= ctx.begin) { ctx.finished = true; return; }
    auto sample = [&](uint64_t pos, uint16_t c) {
        const size_t index = static_cast<size_t>(pos) * d.channels + c;
        return d.format == AudioSampleFormat::F32
            ? static_cast<const float*>(d.pcmData)[index]
            : static_cast<const int16_t*>(d.pcmData)[index] / 32768.0f;
    };
    for (uint32_t f = 0; f < frames; ++f) {
        if (ctx.positionFrames >= end) {
            if (!ctx.loop) { ctx.finished = true; break; }
            ctx.positionFrames = ctx.begin + (ctx.positionFrames - ctx.begin) % (end - ctx.begin);
        }
        const auto pos = ctx.positionFrames;
        const auto next = pos + 1 < end ? pos + 1 : (ctx.loop ? ctx.begin : pos);
        const float t = static_cast<float>(ctx.fractionalAccum);
        float gain = 1.0f;
        if (ctx.fadeOut && !ctx.loop)
            gain = std::min(1.0f, static_cast<float>(end - pos) / ctx.fadeOut);
        for (uint16_t c = 0; c < 2; ++c) {
            const uint16_t sc = d.channels == 1 ? 0 : c;
            const float spatial = d.channels == 1 ? ctx.gains.monoGain
                : (c == 0 ? ctx.gains.leftGain : ctx.gains.rightGain);
            buffer[2*f+c] = (sample(pos,sc) + (sample(next,sc)-sample(pos,sc))*t) * spatial * gain;
        }
        ctx.fractionalAccum += ctx.effectiveScale;
        const auto steps = static_cast<uint64_t>(ctx.fractionalAccum);
        ctx.positionFrames += steps;
        ctx.fractionalAccum -= steps;
    }
    if (!ctx.loop && ctx.positionFrames >= end) ctx.finished = true;
}

static bool pullOneFrame(StreamSourceContext& ctx, float* out) {
    float frame[2] = { 0.0f, 0.0f };
    const uint32_t got = ctx.ring->read(frame, 1);
    if (got == 0) {
        ctx.underrunLatched = true;
        // Still advance the media cursor: AudioMaster sync (AYVideo) must
        // keep moving through silence, otherwise video freezes waiting for
        // PCM that has not arrived yet.
        ++ctx.positionFrames;
        return false;
    }
    ++ctx.positionFrames;
    out[0] = frame[0];
    if (ctx.srcChannels == 2) {
        out[1] = frame[1];
    } else {
        out[1] = frame[0];
    }
    return true;
}

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

struct EnvelopeContext {
    AudioMixer::VoiceSourceFn source;
    void* data;
    VoiceRuntime* voice;
    bool mono=false;
};
void renderEnvelope(void* data, uint32_t index, float* buffer, uint32_t frames) {
    auto& ctx = *static_cast<EnvelopeContext*>(data);
    auto& v = *ctx.voice;
    if (v.paused) return;
    ctx.source(ctx.data, index, buffer, frames);
    for (uint32_t f = 0; f < frames; ++f) {
        float gain = v.attackFrames ? std::min(1.0f, static_cast<float>(v.ageFrames + 1) / v.attackFrames) : 1.0f;
        if (v.releaseFrames) {
            gain = v.releaseStartGain * static_cast<float>(v.releaseRemaining) / v.releaseFrames;
            if (v.releaseRemaining) --v.releaseRemaining;
        }
        v.envelopeGain = gain;
        buffer[2*f] *= gain; buffer[2*f+1] *= gain;
        ++v.ageFrames;
    }
    if (ctx.mono) for (uint32_t f=0;f<frames;++f) buffer[f]=(buffer[2*f]+buffer[2*f+1])*0.5f;
}
} // namespace

// ===== AudioEngine =====

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize(const AudioSettings& settings) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (_initialized) {
        return true;
    }
    if (!settings.sampleRate || (settings.channels!=1 && settings.channels!=2) || !settings.maxVoices || settings.maxVoices>64) return false;
    _settings = settings;

    if (!_commands.initialize(settings.commandQueueCapacity)) {
        return false;
    }
    if (!_clips.initialize(settings.maxClips)) {
        _commands.shutdown();
        return false;
    }
    if (!_mixer.initialize(settings.maxVoices, settings.channels, settings.sampleRate)) {
        _clips.clear();
        _commands.shutdown();
        return false;
    }
    // v2.0b: copy ducking + reverb settings from AudioSettings into the mixer.
    _mixer.setDucking(settings.duckDepthDb,
                      settings.duckAttackPerSec,
                      settings.duckReleasePerSec);
    _mixer.setMasterReverbReturn(settings.reverbReturn);

    _voices.assign(settings.maxVoices, VoiceRuntime{});
    _voiceCapacity = settings.maxVoices;
    _lifecycleGeneration = ++gAudioLifecycle;
    _engineFrame = 0;
    _listener = ListenerParams{};
    _paused = false;
    _timeScale = 1.0f;
    _dopplerScale = 1.0f;
    _streamUnderrunCount.store(0, std::memory_order_relaxed);

    // Pick a backend: honor an explicit setBackend() install, otherwise
    // construct from settings.backend (Null / Mock / Miniaudio). Header
    // contract is "initialize picks a backend" — without this, Miniaudio
    // in AudioSettings was a no-op and Demo stayed silent with name=None.
    if (_backend == nullptr) {
        _backend = makeBackend(settings.backend);
        if (_backend == nullptr) {
            return false;
        }
    }
    {
        // Seed desired rate/channels before start; device may renegotiate.
        AudioBackendInfo startInfo{};
        startInfo.sampleRate = settings.sampleRate > 0 ? settings.sampleRate : 48000;
        startInfo.channels = settings.channels > 0 ? settings.channels : 2;
        startInfo.bitsPerSample = 32;
        // Bind before start so the first device callback can reach the engine.
        _backend->bindAudioEngine(this);
        if (!_backend->start(startInfo)) {
            _backend->bindAudioEngine(nullptr);
            _backend.reset();
            return false;
        }
        // Device may negotiate a different rate; prefer the live describe().
        _backendInfo = _backend->describe();
        if (_backendInfo.sampleRate > 0
            && _backendInfo.sampleRate != _settings.sampleRate) {
            _settings.sampleRate = _backendInfo.sampleRate;
            // Mixer was initialized with the requested rate; sampleRate field
            // is informational for registerClip resample — mixer block size
            // stays valid as long as channels match.
        }
    }

    _initialized = true;
    return true;
}

void AudioEngine::shutdown() {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
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
    _lifecycleGeneration = ++gAudioLifecycle;

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
    publishOutputLevels({});
    _clips.clear();
    _commands.shutdown();

    _initialized = false;
    _paused = false;
    _timeScale = 1.0f;
    _dopplerScale = 1.0f;
}

void AudioEngine::setBackend(std::unique_ptr<IAudioBackend> backend) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (_initialized) {
        // Hot-swap is not supported in v0; require shutdown first.
        return;
    }
    _backend = std::move(backend);
}

AudioClipId AudioEngine::registerClip(const AudioClipDesc& desc) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return InvalidClipId;
    }
    if (!desc.pcmData || !desc.frameCount || !desc.sampleRate || (desc.channels!=1 && desc.channels!=2)) return InvalidClipId;
    AudioClipDesc normalized = desc;
    std::vector<uint8_t> resampleScratch;

    // Sample-rate policy (§3.2): engine target rate; linear-resample on
    // register so renderVoice never uses pitch as a rate substitute.
    const uint32_t targetRate = _settings.sampleRate > 0 ? _settings.sampleRate : 48000;
    if (desc.sampleRate > 0 && desc.sampleRate != targetRate && desc.pcmData != nullptr
        && desc.frameCount > 0) {
        const uint16_t ch = desc.channels;
        const double ratio = static_cast<double>(targetRate) / static_cast<double>(desc.sampleRate);
        const uint32_t outFrames = static_cast<uint32_t>(
            static_cast<double>(desc.frameCount) * ratio + 0.5);
        if (outFrames == 0) {
            return InvalidClipId;
        }
        if (desc.format == AudioSampleFormat::S16) {
            const auto* src = static_cast<const int16_t*>(desc.pcmData);
            resampleScratch.resize(static_cast<size_t>(outFrames) * ch * sizeof(int16_t));
            auto* dst = reinterpret_cast<int16_t*>(resampleScratch.data());
            for (uint32_t i = 0; i < outFrames; ++i) {
                const double srcPos = static_cast<double>(i) / ratio;
                uint32_t i0 = static_cast<uint32_t>(srcPos);
                if (i0 >= desc.frameCount) {
                    i0 = desc.frameCount - 1;
                }
                uint32_t i1 = std::min(i0 + 1, desc.frameCount - 1);
                const float t = static_cast<float>(srcPos - static_cast<double>(i0));
                for (uint16_t c = 0; c < ch; ++c) {
                    const float a = static_cast<float>(src[i0 * ch + c]);
                    const float b = static_cast<float>(src[i1 * ch + c]);
                    float s = a + (b - a) * t;
                    int32_t v = static_cast<int32_t>(s < 0.f ? s - 0.5f : s + 0.5f);
                    v = std::clamp(v, -32768, 32767);
                    dst[i * ch + c] = static_cast<int16_t>(v);
                }
            }
            normalized.pcmData = resampleScratch.data();
            normalized.frameCount = outFrames;
            normalized.sampleRate = targetRate;
        } else if (desc.format == AudioSampleFormat::F32) {
            const auto* src = static_cast<const float*>(desc.pcmData);
            resampleScratch.resize(static_cast<size_t>(outFrames) * ch * sizeof(float));
            auto* dst = reinterpret_cast<float*>(resampleScratch.data());
            for (uint32_t i = 0; i < outFrames; ++i) {
                const double srcPos = static_cast<double>(i) / ratio;
                uint32_t i0 = static_cast<uint32_t>(srcPos);
                if (i0 >= desc.frameCount) {
                    i0 = desc.frameCount - 1;
                }
                uint32_t i1 = std::min(i0 + 1, desc.frameCount - 1);
                const float t = static_cast<float>(srcPos - static_cast<double>(i0));
                for (uint16_t c = 0; c < ch; ++c) {
                    const float a = src[i0 * ch + c];
                    const float b = src[i1 * ch + c];
                    dst[i * ch + c] = a + (b - a) * t;
                }
            }
            normalized.pcmData = resampleScratch.data();
            normalized.frameCount = outFrames;
            normalized.sampleRate = targetRate;
        }
    }

    const AudioClipId id = _clips.registerClip(normalized);
    if (id == InvalidClipId) {
        return InvalidClipId;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::RegisterClip;
    cmd.clipId = id;
    cmd.clipDesc = normalized;
    // Command copies desc by value but pcmData must point at registry storage
    // after register — _applyCommand uses clipId to skip re-register when present.
    if (const ClipSlot* slot = _clips.find(id)) {
        cmd.clipDesc = slot->desc;
    }
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    return id;
}

void AudioEngine::releaseClip(AudioClipId id) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::ReleaseClip;
    cmd.clipId = id;
    cmd.clip = id;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    for (auto& v : _voices) if (v.sourceKind == VoiceSourceKind::Clip && v.clip == id) v.playing = false;
    _clips.releaseClip(id);
}

VoiceHandle AudioEngine::play(AudioClipId clip,
                              AudioBus bus,
                              const SpatializationParams& spatial,
                              float volume,
                              bool loop) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return InvalidVoice;
    }
    if (clip == InvalidClipId || _clips.find(clip) == nullptr) {
        return InvalidVoice;
    }

    // Allocate immediately so the caller can use the handle this frame.
    // Assign a monotonic voiceId so the audio thread's _applyCommand can
    // locate the slot we just populated and skip re-allocation.
    const uint64_t vid = _nextVoiceId++;
    VoiceHandle voice = _allocateVoice();
    if (voice == InvalidVoice) {
        return InvalidVoice;
    }
    VoiceRuntime& slot = _voices[voice - 1];
    slot = VoiceRuntime{};
    slot.voiceId = vid;
    slot.clip = clip;
    slot.bus = bus;
    slot.spatial = spatial;
    slot.volume = std::clamp(volume, 0.0f, 8.0f);
    slot.pitch = 1.0f;
    slot.loop = loop;
    slot.playing = true;
    slot.startFrame = _engineFrame;
    slot.positionFrames = 0;

    // Audio-thread breadcrumb carries the same identity so _applyCommand
    // sees the slot as already populated and just no-ops.
    AudioCommand cmd{};
    cmd.type = AudioCommandType::Play;
    cmd.voiceId = vid;
    cmd.voice = vid;
    cmd.clip = clip;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = slot.volume;
    cmd.loop = loop ? 1 : 0;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    return vid;
}

void AudioEngine::stop(VoiceHandle voice) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::Stop;
    cmd.voice = voice;
    cmd.voiceId = voice;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    uint32_t idx;
    if (!_findVoiceById(voice, idx)) return;
    if (idx < _voices.size()) {
        _voices[idx].playing = false;
        _voices[idx].positionFrames = 0;
    }
}

void AudioEngine::stopAll() {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::StopAll;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    for (auto& voice : _voices) {
        voice.playing = false;
        voice.positionFrames = 0;
    }
}

void AudioEngine::setVoiceVolume(VoiceHandle voice, float volume) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoiceVolume;
    cmd.voice = voice;
    cmd.voiceId = voice;
    cmd.scalar = std::clamp(volume, 0.0f, 8.0f);
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    uint32_t idx;
    if (!_findVoiceById(voice, idx)) return;
    if (idx < _voices.size()) {
        _voices[idx].volume = cmd.scalar;
    }
}

void AudioEngine::setVoicePitch(VoiceHandle voice, float pitch) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoicePitch;
    cmd.voice = voice;
    cmd.voiceId = voice;
    cmd.scalar = pitch;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    uint32_t idx;
    if (!_findVoiceById(voice, idx)) return;
    if (idx < _voices.size()) {
        _voices[idx].pitch = std::isfinite(pitch) ? std::clamp(pitch,0.5f,4.0f) : 1.0f;
    }
}

bool AudioEngine::setVoicePositionFrames(VoiceHandle voice,
                                         uint64_t positionFrames) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || voice == InvalidVoice) return false;
    uint32_t idx;
    if (!_findVoiceById(voice, idx)) return false;
    if (idx >= _voices.size()) return false;
    VoiceRuntime& slot = _voices[idx];
    if (!slot.playing || slot.sourceKind != VoiceSourceKind::Clip) return false;
    const ClipSlot* clip = _clips.find(slot.clip);
    if (clip == nullptr || clip->desc.frameCount == 0u) return false;
    const uint64_t clamped = std::min<uint64_t>(
        positionFrames, static_cast<uint64_t>(clip->desc.frameCount - 1u));

    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoicePosition;
    cmd.voice = voice;
    cmd.voiceId = slot.voiceId;
    cmd.positionFrames = clamped;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    slot.positionFrames = clamped;
    slot.sourceFraction = 0.0;
    // A range preview/seek must continue the clip fade at the source playhead.
    const float scale=std::clamp(slot.pitch*_timeScale.load(),0.5f,4.0f);
    slot.ageFrames=clamped>=slot.rangeBegin ? static_cast<uint64_t>((clamped-slot.rangeBegin)/scale) : 0;
    slot.envelopeGain=slot.attackFrames ? std::min(1.0f,static_cast<float>(slot.ageFrames)/slot.attackFrames) : 1.0f;
    return true;
}

void AudioEngine::setVoiceSpatial(VoiceHandle voice, const SpatializationParams& spatial) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || voice == InvalidVoice) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetVoiceSpatialParams;
    cmd.voice = voice;
    cmd.voiceId = voice;
    cmd.spatial = spatial;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    uint32_t idx;
    if (!_findVoiceById(voice, idx)) return;
    if (idx < _voices.size()) {
        _voices[idx].spatial = spatial;
    }
}

void AudioEngine::setBusGain(AudioBus bus, float linearGain) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetBusGain;
    cmd.bus = bus;
    cmd.scalar = linearGain;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _mixer.setBusGain(bus, linearGain);
}

void AudioEngine::setMasterGain(float linearGain) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetMasterGain;
    cmd.scalar = linearGain;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _mixer.setMasterGain(linearGain);
}

void AudioEngine::setListener(const ListenerParams& listener) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetListener;
    cmd.listener = listener;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _listener = listener;
}

void AudioEngine::pause() {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::PauseEngine;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _paused = true;
}

void AudioEngine::resume() {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::ResumeEngine;
    cmd.appliedSynchronously = true;
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
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
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
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    return static_cast<AudioStreamId>(chosenIdx + 1);
}

void AudioEngine::closeStream(AudioStreamId id) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
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
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);

    // Free the slot. Active voices backed by this stream see `finished` on
    // their next render call (ring drained / closed); they don't crash.
    slot.ring.reset();
    slot.desc = AudioStreamDesc{};
    slot.inUse = false;
}

uint32_t AudioEngine::streamPush(AudioStreamId id, const float* pcm, uint32_t frameCount) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || id == InvalidStreamId || pcm == nullptr || frameCount == 0) {
        return 0;
    }
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return 0;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return 0;
    return slot.ring->write(pcm, frameCount);
}

void AudioEngine::markStreamEndOfStream(AudioStreamId id) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized || id == InvalidStreamId) {
        return;
    }
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return;
    slot.ring->markEndOfStream();
}

VoiceHandle AudioEngine::playStream(AudioStreamId stream,
                                     AudioBus bus,
                                     const SpatializationParams& spatial,
                                     float volume,
                                     bool loop) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return InvalidVoice;
    }
    if (stream == InvalidStreamId) return InvalidVoice;
    const uint32_t idx = stream - 1;
    if (idx >= kMaxStreams) return InvalidVoice;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return InvalidVoice;

    const uint64_t vid = _nextVoiceId++;
    VoiceHandle voice = _allocateVoice();
    if (voice == InvalidVoice) {
        return InvalidVoice;
    }
    VoiceRuntime& v = _voices[voice - 1];
    v = VoiceRuntime{};
    v.voiceId = vid;
    v.sourceKind = VoiceSourceKind::Stream;
    v.stream = stream;
    v.bus = bus;
    v.spatial = spatial;
    v.volume = std::clamp(volume, 0.0f, 8.0f);
    v.pitch = 1.0f;
    v.loop = loop;
    v.playing = true;
    v.startFrame = _engineFrame;
    v.positionFrames = 0;

    AudioCommand cmd{};
    cmd.type = AudioCommandType::PlayStream;
    cmd.voiceId = vid;
    cmd.voice = vid;
    cmd.stream = stream;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = v.volume;
    cmd.loop = loop ? 1 : 0;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    return vid;
}

void AudioEngine::setTimeScale(float scale) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    // Clamp to a safe musical range; the audio thread reads once per
    // voice per render block so values outside [0.25, 4.0] make no sense
    // and would risk silent aliasing.
    _timeScale = std::isfinite(scale) ? std::clamp(scale, 0.25f, 4.0f) : 1.0f;
}

// ===== v2.0b engine-side audio effects =====

void AudioEngine::setDucking(float depthDb, float attackPerSec, float releasePerSec) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetDucking;
    cmd.scalar  = depthDb;
    cmd.scalar2 = attackPerSec;
    cmd.scalar3 = releasePerSec;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _mixer.setDucking(depthDb, attackPerSec, releasePerSec);
}

void AudioEngine::setMasterReverbReturn(float linearReturn) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetReverbReturn;
    cmd.scalar = std::clamp(linearReturn, 0.0f, 1.0f);
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _mixer.setMasterReverbReturn(cmd.scalar);
}

void AudioEngine::setReverbPreset(uint8_t preset) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetReverbPreset;
    cmd.boolFlag = preset;
    cmd.appliedSynchronously = true;
    _commands.tryPush(cmd);
    _mixer.setReverbPreset(preset);
}

void AudioEngine::setDopplerScale(float scale) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    // Game-thread direct write — same pattern as _timeScale / _paused.
    _dopplerScale = std::isfinite(scale) ? std::clamp(scale, 0.0f, 2.0f) : 1.0f;
}

AudioStreamRingBuffer* AudioEngine::streamRing(AudioStreamId id) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (id == InvalidStreamId) return nullptr;
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return nullptr;
    StreamSlot& slot = _streams[idx];
    return slot.inUse ? slot.ring.get() : nullptr;
}

const AudioStreamDesc& AudioEngine::streamDesc(AudioStreamId id) const {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    static const AudioStreamDesc kEmpty{};
    if (id == InvalidStreamId) return kEmpty;
    const uint32_t idx = id - 1;
    if (idx >= kMaxStreams) return kEmpty;
    const StreamSlot& slot = _streams[idx];
    return slot.inUse ? slot.desc : kEmpty;
}

void AudioEngine::submitFrame(float /*deltaTime*/) {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (!_initialized) {
        return;
    }
    // SPSC command queue: exactly one consumer may pop. With a real device
    // the Miniaudio callback thread owns drain via render(). Popping here on
    // the game thread races that callback (torn commands, double-apply, lost
    // Stop/SetGain) and has caused "stream open OK but silent" Gallery bugs.
    // Null/Mock have no audio thread — drain on the game tick instead.
    const bool realDevice = (_backend != nullptr && _backend->isRealDevice());
    if (!realDevice) {
        AudioCommand cmd{};
        int safety = 4096;
        while (safety-- > 0 && _commands.tryPop(cmd)) {
            _applyCommand(cmd);
        }
    }
    ++_engineFrame;
}

void AudioEngine::render(float* output, uint32_t frameCount) {
    if (!output || !frameCount) return;
    std::unique_lock<std::recursive_mutex> stateLock(_stateMutex, std::try_to_lock);
    if (!stateLock.owns_lock()) {
        publishOutputLevels({});
        std::memset(output,0,static_cast<size_t>(frameCount)*_settings.channels*sizeof(float));
        return;
    }
    if (!_initialized || output == nullptr || frameCount == 0) {
        publishOutputLevels({});
        std::memset(output,0,static_cast<size_t>(frameCount)*_settings.channels*sizeof(float));
        return;
    }
    // Drain commands so audio-thread-visible state is up to date.
    AudioCommand cmd{};
    int safety = 4096;
    while (safety-- > 0 && _commands.tryPop(cmd)) {
        _applyCommand(cmd);
    }

    if (_paused) {
        publishOutputLevels({});
        std::memset(output, 0, frameCount * _settings.channels * sizeof(float));
        return;
    }

    if (frameCount>4096) {
        AudioOutputLevels total;
        for (uint32_t offset=0;offset<frameCount;) {
            const uint32_t count=std::min<uint32_t>(4096,frameCount-offset);
            render(output+static_cast<size_t>(offset)*_settings.channels,count); offset+=count;
            const auto part=outputLevels();
            total.left=std::max(total.left,part.left);total.right=std::max(total.right,part.right);total.clipped|=part.clipped;
        }
        publishOutputLevels(total);
        return;
    }
    // Build per-voice render context list. Voices that finished in a prior
    // render call are recycled at the start of this one.
    constexpr uint32_t kMaxVoicesPerRender = 64;
    if (_voices.size() > kMaxVoicesPerRender) {
        publishOutputLevels({});
        return; // safety guard; settings.maxVoices should be ≤ 64 for v0
    }

    VoiceSourceContext clipContexts[kMaxVoicesPerRender]{};
    StreamSourceContext streamContexts[kMaxVoicesPerRender]{};
    AudioMixer::VoiceSourceFn sources[kMaxVoicesPerRender]{};
    void* userData[kMaxVoicesPerRender]{};
    VoiceRuntime voiceSnapshot[kMaxVoicesPerRender]{};
    EnvelopeContext envelopes[kMaxVoicesPerRender]{};
    uint32_t activeCount = 0;

    // Snapshot _timeScale once per render call so all voices see a coherent
    // value. Float writes on x86/ARM are atomic for naturally-aligned
    // 4-byte loads; we read once. Same pattern for _dopplerScale in v2.0b.
    const float timeScale = _timeScale;
    const float dopplerUserScale = _dopplerScale;

    for (uint32_t i = 0; i < _voices.size(); ++i) {
        VoiceRuntime& slot = _voices[i];
        if (!slot.playing || slot.paused) {
            continue;
        }
        SpatialGains gains = AudioSpatializer::compute(_listener, slot.spatial, _settings.channels);

        // v2.0b doppler: factor = AudioSpatializer::dopplerFactor(listener, voice)
        // is in [0.5, 2.0]. Multiplied by user `dopplerUserScale` (default 1.0)
        // so an author can disable doppler via setDopplerScale(0).
        const float doppler = 1.0f + (AudioSpatializer::dopplerFactor(_listener, slot.spatial) - 1.0f) * dopplerUserScale;

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
            // Carry the cumulative source-frame cursor across render blocks
            // (stack context is otherwise zeroed each call).
            sctx.positionFrames = slot.positionFrames;
            // v1.5: voice.pitch * timeScale feeds the source-step scalar
            // inside renderVoiceStream. The accumulator survives across
            // calls on the stack array (zeroed at first entry, which is
            // a fresh context — see render() shape).
            // v2.0b: doppler factor folded in. Clamp raised to [0.5, 4.0]
            // so the doppler [0.5, 2.0] envelope isn't truncated.
            sctx.effectiveScale = std::clamp(slot.pitch * timeScale * doppler, 0.5f, 4.0f);
            sctx.fractionalAccum = slot.sourceFraction;
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
        // Preserve fractional source phase across blocks, including non-integer pitch.
        ctx.effectiveScale = std::clamp(slot.pitch * timeScale * doppler, 0.5f, 4.0f);
        ctx.fractionalAccum = slot.sourceFraction;
        ctx.begin = slot.rangeBegin; ctx.end = slot.rangeEnd; ctx.fadeOut = slot.fadeOutFrames;
        sources[activeCount] = &renderVoicePcm;
        userData[activeCount] = &clipContexts[activeCount];
        voiceSnapshot[activeCount] = slot;
        ++activeCount;
    }

    for (uint32_t i=0; i<activeCount; ++i) {
        envelopes[i] = {sources[i], userData[i], &voiceSnapshot[i], _settings.channels==1};
        sources[i] = &renderEnvelope; userData[i] = &envelopes[i];
    }
    _mixer.mix(output, frameCount, voiceSnapshot, sources, userData, activeCount);
    publishOutputLevels(_mixer.outputLevels());

    // v2.0b: integrate duck envelope AFTER mix() (which populated the bus
    // probe inside its per-voice loop). dt = frames / sampleRate seconds.
    if (_settings.sampleRate > 0) {
        const float dt = static_cast<float>(frameCount) / static_cast<float>(_settings.sampleRate);
        _mixer.integrateDuckEnvelope(dt);
    }

    // Commit read heads and prune finished voices. Both context arrays use
    // a parallel layout indexed by `i`; we look at snap.sourceKind to pick
    // the right finished flag.
    for (uint32_t i = 0; i < activeCount; ++i) {
        const VoiceRuntime& snap = voiceSnapshot[i];
        // Map back from snapshot to live voice slot via the monotonic
        // voiceId. voiceId is unique per play() / playStream() call and
        // survives stop() (the counter is engine-wide), so two voices
        // played in the same engine frame no longer collide the way they
        // did when startFrame was the identity (see A1 in the audit).
        for (uint32_t v = 0; v < _voices.size(); ++v) {
            if (!_voices[v].playing || _voices[v].voiceId != snap.voiceId) continue;
            _voices[v].ageFrames = snap.ageFrames;
            _voices[v].envelopeGain = snap.envelopeGain;
            _voices[v].releaseRemaining = snap.releaseRemaining;
            if (snap.releaseFrames && !snap.releaseRemaining) _voices[v].playing = false;
            if (snap.sourceKind == VoiceSourceKind::Stream) {
                if (_voices[v].sourceKind != VoiceSourceKind::Stream) continue;
                const StreamSourceContext& sctx = streamContexts[i];
                // Persist source frames drained so voicePositionFrames()
                // can drive audio-master sync clocks (AYVideo V2).
                _voices[v].positionFrames = sctx.positionFrames;
                _voices[v].sourceFraction = sctx.fractionalAccum;
                if (sctx.underrunLatched) {
                    _streamUnderrunCount.fetch_add(1, std::memory_order_relaxed);
                }
                if (sctx.finished) {
                    _voices[v].playing = false;
                }
                break;
            } else {
                if (_voices[v].sourceKind != VoiceSourceKind::Clip) continue;
                const VoiceSourceContext& ctx = clipContexts[i];
                if (ctx.finished && !ctx.loop) {
                    _voices[v].playing = false;
                } else {
                    _voices[v].positionFrames = ctx.positionFrames;
                    _voices[v].sourceFraction = ctx.fractionalAccum;
                }
                break;
            }
        }
    }
}

uint32_t AudioEngine::activeVoiceCount() const {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    uint32_t count = 0;
    for (const auto& v : _voices) {
        if (v.playing) ++count;
    }
    return count;
}

const VoiceRuntime* AudioEngine::voiceAt(uint32_t index) const {
    std::lock_guard<std::recursive_mutex> stateLock(_stateMutex);
    if (index >= _voices.size()) return nullptr;
    return &_voices[index];
}

uint64_t AudioEngine::voicePositionFrames(VoiceHandle voice) const {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t index;
    return _findVoiceById(voice,index) && _voices[index].playing ? _voices[index].positionFrames : 0;
}
VoiceHandle AudioEngine::_allocateVoice() {
    for (uint32_t i=0; i<_voices.size(); ++i) if (!_voices[i].playing) return i+1;
    return InvalidVoice;
}

bool AudioEngine::_findVoiceById(uint64_t voiceId, uint32_t& outSlot) const {
    if (voiceId == 0) return false;
    for (uint32_t i = 0; i < _voices.size(); ++i) {
        if (_voices[i].voiceId == voiceId) {
            outSlot = i;
            return true;
        }
    }
    return false;
}

void AudioEngine::_applyCommand(const AudioCommand& cmd) {
    if (cmd.appliedSynchronously) return;
    switch (cmd.type) {
        case AudioCommandType::Play: break; // public play already allocated its identity
        case AudioCommandType::Stop: {
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].playing = false;
                _voices[idx].positionFrames = 0;
                break;
            }
            uint32_t vi;
            if (!_findVoiceById(cmd.voice, vi)) break;
            if (vi < _voices.size()) {
                _voices[vi].playing = false;
                _voices[vi].positionFrames = 0;
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
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].volume = std::clamp(cmd.scalar, 0.0f, 8.0f);
                break;
            }
            // Fallback for older callers / tests that didn't carry voiceId.
            uint32_t vi;
            if (!_findVoiceById(cmd.voice, vi)) break;
            if (vi < _voices.size()) {
                _voices[vi].volume = std::clamp(cmd.scalar, 0.0f, 8.0f);
            }
            break;
        }
        case AudioCommandType::SetVoicePitch: {
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].pitch = cmd.scalar;
                break;
            }
            uint32_t vi;
            if (!_findVoiceById(cmd.voice, vi)) break;
            if (vi < _voices.size()) {
                _voices[vi].pitch = cmd.scalar;
            }
            break;
        }
        case AudioCommandType::SetVoicePosition: {
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                VoiceRuntime& voice = _voices[idx];
                if (voice.sourceKind != VoiceSourceKind::Clip) break;
                const ClipSlot* clip = _clips.find(voice.clip);
                if (clip != nullptr && clip->desc.frameCount != 0u) {
                    voice.positionFrames = std::min<uint64_t>(
                        cmd.positionFrames,
                        static_cast<uint64_t>(clip->desc.frameCount - 1u));
                }
                break;
            }
            uint32_t vi;
            if (!_findVoiceById(cmd.voice, vi)) break;
            if (vi < _voices.size()
                && _voices[vi].sourceKind == VoiceSourceKind::Clip) {
                const ClipSlot* clip = _clips.find(_voices[vi].clip);
                if (clip != nullptr && clip->desc.frameCount != 0u) {
                    _voices[vi].positionFrames = std::min<uint64_t>(
                        cmd.positionFrames,
                        static_cast<uint64_t>(clip->desc.frameCount - 1u));
                }
            }
            break;
        }
        case AudioCommandType::SetVoiceSpatialParams: {
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].spatial = cmd.spatial;
                break;
            }
            uint32_t vi;
            if (!_findVoiceById(cmd.voice, vi)) break;
            if (vi < _voices.size()) {
                _voices[vi].spatial = cmd.spatial;
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
        case AudioCommandType::RegisterClip: {
            // Game thread already registered with the same clipId (carried
            // in cmd.clipId); if find() hits, this is a no-op replay.
            if (cmd.clipId != InvalidClipId && _clips.find(cmd.clipId) != nullptr) {
                break;
            }
            // Pathological: game thread's register failed earlier; do the
            // best we can — register on the audio thread using whatever
            // free slot is available.
            _clips.registerClip(cmd.clipDesc);
            break;
        }
        case AudioCommandType::ReleaseClip: {
            if (cmd.clipId != InvalidClipId) {
                _clips.releaseClip(cmd.clipId);
                break;
            }
            if (cmd.clip != InvalidClipId) {
                _clips.releaseClip(cmd.clip);
            }
            break;
        }
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
        case AudioCommandType::PlayStream: break; // public play already allocated its identity
        case AudioCommandType::SetDucking: {
            // Mirror the engine-side immediate write.
            _mixer.setDucking(cmd.scalar, cmd.scalar2, cmd.scalar3);
            break;
        }
        case AudioCommandType::SetReverbReturn: {
            _mixer.setMasterReverbReturn(cmd.scalar);
            break;
        }
        case AudioCommandType::SetReverbPreset: {
            _mixer.setReverbPreset(cmd.boolFlag);
            break;
        }
        case AudioCommandType::SetDopplerScale: {
            // _dopplerScale is game-thread only — same direct-write pattern
            // as _timeScale. The command exists for symmetry/debug but the
            // engine-side setter already wrote to _dopplerScale on the
            // game thread, so audio-thread replay is a no-op.
            (void)cmd;
            break;
        }
    }
}

bool AudioEngine::voiceState(VoiceHandle handle, VoiceRuntime& out) const {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i;
    if (!_findVoiceById(handle,i) || !_voices[i].playing) return false;
    out = _voices[i]; return true;
}
bool AudioEngine::voiceStateAt(uint32_t index, VoiceRuntime& out) const {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    if (index >= _voices.size()) return false;
    out = _voices[index]; return true;
}
const VoiceRuntime* AudioEngine::voiceForHandle(VoiceHandle handle) const {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i; return _findVoiceById(handle,i) ? &_voices[i] : nullptr;
}
void AudioEngine::setVoicePaused(VoiceHandle handle, bool paused) {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i; if (_findVoiceById(handle,i)) _voices[i].paused=paused;
}
void AudioEngine::releaseVoice(VoiceHandle handle, float seconds) {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i; if (!_findVoiceById(handle,i) || !_voices[i].playing) return;
    if (!std::isfinite(seconds) || seconds <= 0) { stop(handle); return; }
    auto& v=_voices[i];
    if (v.releaseFrames) return;
    v.releaseFrames=std::max<uint64_t>(1,static_cast<uint64_t>(std::min(seconds,10.0f)*_settings.sampleRate));
    v.releaseRemaining=v.releaseFrames; v.releaseStartGain=v.envelopeGain;
}
bool AudioEngine::setVoicePlaybackRange(VoiceHandle handle, uint64_t begin, uint64_t end) {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i; if (!_findVoiceById(handle,i)) return false;
    auto& v=_voices[i]; const auto* c=_clips.find(v.clip);
    if (!v.playing || v.sourceKind!=VoiceSourceKind::Clip || !c) return false;
    end=std::min<uint64_t>(end,c->desc.frameCount);
    if (begin>=end) return false;
    v.rangeBegin=begin; v.rangeEnd=end; v.positionFrames=begin; v.sourceFraction=0;
    return true;
}
void AudioEngine::setVoiceEnvelope(VoiceHandle handle, float attack, float fadeOut) {
    std::lock_guard<std::recursive_mutex> lock(_stateMutex);
    uint32_t i; if (!_findVoiceById(handle,i)) return;
    auto frames=[&](float sec) { return std::isfinite(sec) ? static_cast<uint64_t>(std::clamp(sec,0.0f,10.0f)*_settings.sampleRate) : 0; };
    _voices[i].attackFrames=frames(attack); _voices[i].fadeOutFrames=frames(fadeOut);
    if (!_voices[i].ageFrames) _voices[i].envelopeGain=_voices[i].attackFrames ? 0.0f : 1.0f;
}
float AudioEngine::masterGain() const { std::lock_guard<std::recursive_mutex> lock(_stateMutex); return _mixer.getMasterGain(); }
float AudioEngine::busGain(AudioBus bus) const { std::lock_guard<std::recursive_mutex> lock(_stateMutex); return _mixer.getBusGain(bus); }

float AudioEngine::reverbReturn() const {std::lock_guard<std::recursive_mutex> lock(_stateMutex);return _mixer.reverbReturn();}
uint8_t AudioEngine::reverbPreset() const {std::lock_guard<std::recursive_mutex> lock(_stateMutex);return _mixer.reverbPreset();}
DuckingState AudioEngine::duckingState() const {std::lock_guard<std::recursive_mutex> lock(_stateMutex);return _mixer.duckState();}
} // namespace ayt::audio
