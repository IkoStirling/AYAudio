#include <AYAudioEngine.h>

#include <AYAudioBackendFactory.h>
#include <AYAudioSpatializer.h>
#include <AYAudioStreamRingBuffer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <vector>

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

    // Block-quantized stepping path for non-identity scale. Each output
    // frame consumes `scale` source frames; the integer part drives how
    // many positions we advance, the fractional part carries into the
    // next call. The output sample is **linearly interpolated** between
    // the position we land on and the next source sample using the
    // residual fractional offset — this is what gives pitch-up smooth
    // tonal transitions instead of nearest-sample repeat. Looping wraps
    // the *position*; interpolation always reads the next valid sample.
    if (desc.format == AudioSampleFormat::F32) {
        const float* data = reinterpret_cast<const float*>(desc.pcmData);
        if (data == nullptr) {
            return;
        }
        // Helpers: read source-frame sample(s) with linear interpolation.
        // `nextPos` advances by 1, wrapping on loop, clamping on EOS.
        auto nextPosF = [&](uint64_t p) -> uint64_t {
            if (p + 1 < totalFrames) return p + 1;
            return ctx->loop ? 0 : p;
        };
        if (srcChannels == 1) {
            for (uint32_t f = 0; f < frameCount; ++f) {
                ctx->fractionalAccum += static_cast<double>(scale);
                int32_t wholeSteps = static_cast<int32_t>(ctx->fractionalAccum);
                if (wholeSteps <= 0) {
                    // Deficit — repeat last output sample, or silence if voice just started.
                    const float last = (f == 0) ? 0.0f : voiceBuffer[2 * (f - 1) + 0];
                    voiceBuffer[2 * f + 0] = last;
                    voiceBuffer[2 * f + 1] = last;
                    continue;
                }
                // Advance wholeSteps source positions; the last advance is
                // the position we sample from. Linear-interpolate against
                // its neighbor using the residual fraction.
                uint64_t samplePos = pos;
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (samplePos >= totalFrames) {
                        if (ctx->loop) { samplePos = 0; } else { ctx->finished = true; break; }
                    }
                    if (s == wholeSteps - 1) {
                        // Land the read head on samplePos.
                    }
                    ++samplePos;
                }
                if (ctx->finished) {
                    // Voice drained mid-step; emit silence for this frame.
                    voiceBuffer[2 * f + 0] = 0.0f;
                    voiceBuffer[2 * f + 1] = 0.0f;
                    pos = samplePos;
                    ctx->fractionalAccum -= static_cast<double>(wholeSteps);
                    continue;
                }
                // samplePos landed one past the read position (because we
                // always ++ it after using). Step back to get the actual
                // sample position we ended on.
                const uint64_t curPos = samplePos - 1;
                const uint64_t nxtPos = nextPosF(curPos);
                const float subFrac = static_cast<float>(
                    ctx->fractionalAccum - static_cast<double>(wholeSteps));
                const float cur = data[curPos];
                const float nxt = data[nxtPos];
                const float v = (cur + (nxt - cur) * subFrac) * monoGain;
                voiceBuffer[2 * f + 0] = v;
                voiceBuffer[2 * f + 1] = v;
                pos = samplePos;
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
                uint64_t samplePos = pos;
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (samplePos >= totalFrames) {
                        if (ctx->loop) { samplePos = 0; } else { ctx->finished = true; break; }
                    }
                    ++samplePos;
                }
                if (ctx->finished) {
                    voiceBuffer[2 * f + 0] = 0.0f;
                    voiceBuffer[2 * f + 1] = 0.0f;
                    pos = samplePos;
                    ctx->fractionalAccum -= static_cast<double>(wholeSteps);
                    continue;
                }
                const uint64_t curPos = samplePos - 1;
                const uint64_t nxtPos = nextPosF(curPos);
                const float subFrac = static_cast<float>(
                    ctx->fractionalAccum - static_cast<double>(wholeSteps));
                const float curL = data[2 * curPos + 0];
                const float curR = data[2 * curPos + 1];
                const float nxtL = data[2 * nxtPos + 0];
                const float nxtR = data[2 * nxtPos + 1];
                voiceBuffer[2 * f + 0] = (curL + (nxtL - curL) * subFrac) * leftGain;
                voiceBuffer[2 * f + 1] = (curR + (nxtR - curR) * subFrac) * rightGain;
                pos = samplePos;
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        }
    } else {
        const int16_t* data = reinterpret_cast<const int16_t*>(desc.pcmData);
        if (data == nullptr) {
            return;
        }
        constexpr float kInvS16 = 1.0f / 32768.0f;
        auto nextPosI = [&](uint64_t p) -> uint64_t {
            if (p + 1 < totalFrames) return p + 1;
            return ctx->loop ? 0 : p;
        };
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
                uint64_t samplePos = pos;
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (samplePos >= totalFrames) {
                        if (ctx->loop) { samplePos = 0; } else { ctx->finished = true; break; }
                    }
                    ++samplePos;
                }
                if (ctx->finished) {
                    voiceBuffer[2 * f + 0] = 0.0f;
                    voiceBuffer[2 * f + 1] = 0.0f;
                    pos = samplePos;
                    ctx->fractionalAccum -= static_cast<double>(wholeSteps);
                    continue;
                }
                const uint64_t curPos = samplePos - 1;
                const uint64_t nxtPos = nextPosI(curPos);
                const float subFrac = static_cast<float>(
                    ctx->fractionalAccum - static_cast<double>(wholeSteps));
                const float cur = static_cast<float>(data[curPos]) * kInvS16;
                const float nxt = static_cast<float>(data[nxtPos]) * kInvS16;
                const float v = (cur + (nxt - cur) * subFrac) * monoGain;
                voiceBuffer[2 * f + 0] = v;
                voiceBuffer[2 * f + 1] = v;
                pos = samplePos;
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
                uint64_t samplePos = pos;
                for (int32_t s = 0; s < wholeSteps; ++s) {
                    if (samplePos >= totalFrames) {
                        if (ctx->loop) { samplePos = 0; } else { ctx->finished = true; break; }
                    }
                    ++samplePos;
                }
                if (ctx->finished) {
                    voiceBuffer[2 * f + 0] = 0.0f;
                    voiceBuffer[2 * f + 1] = 0.0f;
                    pos = samplePos;
                    ctx->fractionalAccum -= static_cast<double>(wholeSteps);
                    continue;
                }
                const uint64_t curPos = samplePos - 1;
                const uint64_t nxtPos = nextPosI(curPos);
                const float subFrac = static_cast<float>(
                    ctx->fractionalAccum - static_cast<double>(wholeSteps));
                const float curL = static_cast<float>(data[2 * curPos + 0]) * kInvS16;
                const float curR = static_cast<float>(data[2 * curPos + 1]) * kInvS16;
                const float nxtL = static_cast<float>(data[2 * nxtPos + 0]) * kInvS16;
                const float nxtR = static_cast<float>(data[2 * nxtPos + 1]) * kInvS16;
                voiceBuffer[2 * f + 0] = (curL + (nxtL - curL) * subFrac) * leftGain;
                voiceBuffer[2 * f + 1] = (curR + (nxtR - curR) * subFrac) * rightGain;
                pos = samplePos;
                ctx->fractionalAccum -= static_cast<double>(wholeSteps);
            }
        }
    }

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
    _nextVoiceId = 1;
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
    _dopplerScale = 1.0f;
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
    _commands.tryPush(cmd);
    return id;
}

void AudioEngine::releaseClip(AudioClipId id) {
    if (!_initialized) {
        return;
    }
    AudioCommand cmd{};
    cmd.type = AudioCommandType::ReleaseClip;
    cmd.clipId = id;
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

    // Allocate immediately so the caller can use the handle this frame.
    // Assign a monotonic voiceId so the audio thread's _applyCommand can
    // locate the slot we just populated and skip re-allocation.
    const uint64_t vid = ++_nextVoiceId;
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
    cmd.voice = voice;
    cmd.clip = clip;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = slot.volume;
    cmd.loop = loop ? 1 : 0;
    _commands.tryPush(cmd);
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
    cmd.voiceId = (voice >= 1 && voice - 1 < _voices.size()) ? _voices[voice - 1].voiceId : 0;
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
    cmd.voiceId = (voice >= 1 && voice - 1 < _voices.size()) ? _voices[voice - 1].voiceId : 0;
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

void AudioEngine::markStreamEndOfStream(AudioStreamId id) {
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
    if (!_initialized) {
        return InvalidVoice;
    }
    if (stream == InvalidStreamId) return InvalidVoice;
    const uint32_t idx = stream - 1;
    if (idx >= kMaxStreams) return InvalidVoice;
    StreamSlot& slot = _streams[idx];
    if (!slot.inUse || !slot.ring) return InvalidVoice;

    const uint64_t vid = ++_nextVoiceId;
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
    cmd.voice = voice;
    cmd.stream = stream;
    cmd.bus = bus;
    cmd.spatial = spatial;
    cmd.scalar = v.volume;
    cmd.loop = loop ? 1 : 0;
    _commands.tryPush(cmd);
    return voice;
}

void AudioEngine::setTimeScale(float scale) {
    // Clamp to a safe musical range; the audio thread reads once per
    // voice per render block so values outside [0.25, 4.0] make no sense
    // and would risk silent aliasing.
    _timeScale = std::clamp(scale, 0.25f, 4.0f);
}

// ===== v2.0b engine-side audio effects =====

void AudioEngine::setDucking(float depthDb, float attackPerSec, float releasePerSec) {
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetDucking;
    cmd.scalar  = depthDb;
    cmd.scalar2 = attackPerSec;
    cmd.scalar3 = releasePerSec;
    _commands.tryPush(cmd);
    _mixer.setDucking(depthDb, attackPerSec, releasePerSec);
}

void AudioEngine::setMasterReverbReturn(float linearReturn) {
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetReverbReturn;
    cmd.scalar = std::clamp(linearReturn, 0.0f, 1.0f);
    _commands.tryPush(cmd);
    _mixer.setMasterReverbReturn(cmd.scalar);
}

void AudioEngine::setReverbPreset(uint8_t preset) {
    if (!_initialized) return;
    AudioCommand cmd{};
    cmd.type = AudioCommandType::SetReverbPreset;
    cmd.boolFlag = preset;
    _commands.tryPush(cmd);
    _mixer.setReverbPreset(preset);
}

void AudioEngine::setDopplerScale(float scale) {
    // Game-thread direct write — same pattern as _timeScale / _paused.
    _dopplerScale = std::clamp(scale, 0.0f, 2.0f);
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
    // 4-byte loads; we read once. Same pattern for _dopplerScale in v2.0b.
    const float timeScale = _timeScale;
    const float dopplerUserScale = _dopplerScale;

    for (uint32_t i = 0; i < _voices.size(); ++i) {
        VoiceRuntime& slot = _voices[i];
        if (!slot.playing) {
            continue;
        }
        SpatialGains gains = AudioSpatializer::compute(_listener, slot.spatial, _settings.channels);

        // v2.0b doppler: factor = AudioSpatializer::dopplerFactor(listener, voice)
        // is in [0.5, 2.0]. Multiplied by user `dopplerUserScale` (default 1.0)
        // so an author can disable doppler via setDopplerScale(0).
        const float doppler = AudioSpatializer::dopplerFactor(_listener, slot.spatial) * dopplerUserScale;

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
        ctx.effectiveScale = std::clamp(slot.pitch * timeScale * doppler, 0.5f, 4.0f);
        ctx.fractionalAccum = 0.0;
        sources[activeCount] = &renderVoicePcm;
        userData[activeCount] = &clipContexts[activeCount];
        voiceSnapshot[activeCount] = slot;
        ++activeCount;
    }

    _mixer.mix(output, frameCount, voiceSnapshot, sources, userData, activeCount);

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
            if (snap.sourceKind == VoiceSourceKind::Stream) {
                if (_voices[v].sourceKind != VoiceSourceKind::Stream) continue;
                const StreamSourceContext& sctx = streamContexts[i];
                // Persist source frames drained so voicePositionFrames()
                // can drive audio-master sync clocks (AYVideo V2).
                _voices[v].positionFrames = sctx.positionFrames;
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

uint64_t AudioEngine::voicePositionFrames(VoiceHandle voice) const {
    if (voice == InvalidVoice || voice > _voices.size()) {
        return 0;
    }
    return _voices[voice - 1].positionFrames;
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
    return static_cast<VoiceHandle>(_nextVoiceId++);
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
    switch (cmd.type) {
        case AudioCommandType::Play: {
            // The game thread already populated the slot for this voice
            // (play() assigns voiceId and fills fields synchronously). If
            // we find the matching slot here, the audio-thread replay is a
            // no-op — we skip the dual-allocation that older builds did.
            uint32_t idx;
            if (_findVoiceById(cmd.voiceId, idx)) {
                break;
            }
            // Race / corner case: game thread's slot was recycled (stop
            // released it) before this command drained. Allocate a fresh
            // slot and apply the command payload.
            VoiceHandle v = _allocateVoice();
            if (v == InvalidVoice) break;
            VoiceRuntime& slot = _voices[v - 1];
            slot = VoiceRuntime{};
            slot.voiceId = cmd.voiceId ? cmd.voiceId : ++_nextVoiceId;
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
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].playing = false;
                _voices[idx].positionFrames = 0;
                break;
            }
            if (cmd.voice == InvalidVoice) break;
            const uint32_t vi = cmd.voice - 1;
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
            if (cmd.voice == InvalidVoice) break;
            const uint32_t vi = cmd.voice - 1;
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
            if (cmd.voice == InvalidVoice) break;
            const uint32_t vi = cmd.voice - 1;
            if (vi < _voices.size()) {
                _voices[vi].pitch = cmd.scalar;
            }
            break;
        }
        case AudioCommandType::SetVoiceSpatialParams: {
            uint32_t idx;
            if (cmd.voiceId != 0 && _findVoiceById(cmd.voiceId, idx)) {
                _voices[idx].spatial = cmd.spatial;
                break;
            }
            if (cmd.voice == InvalidVoice) break;
            const uint32_t vi = cmd.voice - 1;
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
        case AudioCommandType::PlayStream: {
            // Same dual-write fix as Play: game thread already populated the
            // slot under cmd.voiceId. If we find it, audio-thread replay is
            // a no-op.
            uint32_t idx;
            if (_findVoiceById(cmd.voiceId, idx)) {
                break;
            }
            // Race / recycle path: re-allocate and apply.
            VoiceHandle v = _allocateVoice();
            if (v == InvalidVoice) break;
            VoiceRuntime& slot = _voices[v - 1];
            slot = VoiceRuntime{};
            slot.voiceId = cmd.voiceId ? cmd.voiceId : ++_nextVoiceId;
            slot.sourceKind = VoiceSourceKind::Stream;
            slot.stream = cmd.stream;
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

} // namespace ayt::audio