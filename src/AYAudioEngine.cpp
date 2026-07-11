#include <AYAudioEngine.h>

#include <AYAudioSpatializer.h>
#include "MiniaudioBackend.h"

#include <algorithm>
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
};

// PCM source callback: fills `voiceBuffer` with interleaved float32 frames.
// For mono clips we duplicate to stereo. For stereo clips we apply
// per-channel gains directly. The function is called from the audio thread
// (mixer) so it must be lock-free.
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

    uint64_t pos = ctx->positionFrames;

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
        // S16 path: convert to float on the fly. For v0 we keep this simple
        // and apply the same gains. Real devices usually pick F32; this
        // branch is mainly for unit tests with synthesized S16 buffers.
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

    _mixer.reset();
    _clips.clear();
    _commands.shutdown();

    _initialized = false;
    _paused = false;
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

    VoiceSourceContext contexts[kMaxVoicesPerRender]{};
    AudioMixer::VoiceSourceFn sources[kMaxVoicesPerRender]{};
    void* userData[kMaxVoicesPerRender]{};
    VoiceRuntime voiceSnapshot[kMaxVoicesPerRender]{};
    uint32_t activeCount = 0;

    for (uint32_t i = 0; i < _voices.size(); ++i) {
        VoiceRuntime& slot = _voices[i];
        if (!slot.playing) {
            continue;
        }
        const ClipSlot* clip = _clips.find(slot.clip);
        if (clip == nullptr) {
            slot.playing = false;
            continue;
        }
        SpatialGains gains = AudioSpatializer::compute(_listener, slot.spatial, _settings.channels);
        contexts[activeCount].clip = clip;
        contexts[activeCount].gains = gains;
        contexts[activeCount].positionFrames = slot.positionFrames;
        contexts[activeCount].loop = slot.loop;
        sources[activeCount] = &renderVoicePcm;
        userData[activeCount] = &contexts[activeCount];
        voiceSnapshot[activeCount] = slot;
        ++activeCount;
    }

    _mixer.mix(output, frameCount, voiceSnapshot, sources, userData, activeCount);

    // Commit read heads and prune finished voices.
    for (uint32_t i = 0; i < activeCount; ++i) {
        const VoiceRuntime& snap = voiceSnapshot[i];
        const VoiceSourceContext& ctx = contexts[i];
        // Map back from snapshot to live voice slot. We re-locate by
        // (clip, startFrame) so concurrent engine-side mutations during
        // this render call can't desync the read head.
        for (uint32_t v = 0; v < _voices.size(); ++v) {
            if (_voices[v].playing && _voices[v].clip == snap.clip && _voices[v].startFrame == snap.startFrame) {
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
    }
}

} // namespace ayt::audio