#include <AYAudio/AudioSubSystem.h>

#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioClipRegistry.h>
#include <AYAudio/AudioEmitterSystem.h>
#include <AYAudio/AudioStreamRingBuffer.h>
#include <AYEntity/World.h>

#include <AYResource/assetsDefs/IAudio.h>   // ayt::resource::IAudio
#include <AYResource/Converter/AudioDecoder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ayt::audio
{

AudioSubSystem* AudioSubSystem::s_instance = nullptr;

namespace {

// Map AYResource's bits-per-sample to AYAudio's AudioSampleFormat enum.
// Only 16 and 32 are accepted: 8/24 would need a new engine dispatch path
// (out of scope for v1.1). Returning true means the input is supported;
// outFormat is set on success.
bool mapBitsPerSample(uint32_t bps, AudioSampleFormat& outFormat) {
    switch (bps) {
        case 16: outFormat = AudioSampleFormat::S16; return true;
        case 32: outFormat = AudioSampleFormat::F32; return true;
        default: return false;
    }
}

// Linear-resample interleaved F32 from sourceRate → targetRate.
uint32_t resampleFloatInterleaved(const float* src, uint32_t srcFrames, uint16_t ch,
                                  uint32_t sourceRate, uint32_t targetRate,
                                  float* dst, uint32_t dstCapacityFrames) {
    if (src == nullptr || dst == nullptr || srcFrames == 0 || ch == 0
        || sourceRate == 0 || targetRate == 0 || dstCapacityFrames == 0) {
        return 0;
    }
    if (sourceRate == targetRate) {
        const uint32_t n = std::min(srcFrames, dstCapacityFrames);
        std::memcpy(dst, src, static_cast<size_t>(n) * ch * sizeof(float));
        return n;
    }
    const double ratio = static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    uint32_t outFrames = static_cast<uint32_t>(static_cast<double>(srcFrames) * ratio + 0.5);
    outFrames = std::min(outFrames, dstCapacityFrames);
    if (outFrames == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < outFrames; ++i) {
        const double srcPos = static_cast<double>(i) / ratio;
        uint32_t i0 = static_cast<uint32_t>(srcPos);
        if (i0 >= srcFrames) {
            i0 = srcFrames - 1;
        }
        uint32_t i1 = std::min(i0 + 1, srcFrames - 1);
        const float t = static_cast<float>(srcPos - static_cast<double>(i0));
        for (uint16_t c = 0; c < ch; ++c) {
            const float a = src[i0 * ch + c];
            const float b = src[i1 * ch + c];
            dst[i * ch + c] = a + (b - a) * t;
        }
    }
    return outFrames;
}

} // namespace

AudioSubSystem::AudioSubSystem() {
    s_instance = this;
    _descriptor.name = "Audio";
    _descriptor.basePriority = 600; // see AYEntity/design.md: 600+ reserved for Audio
    _descriptor.timeType = ayt::game::SubSystemDescriptor::TimeType::Unscaled;
    _descriptor.phases = ayt::game::phaseBit(ayt::game::FramePhase::Presentation);
    _descriptor.clock = ayt::game::ClockDomain::Unscaled;
    _descriptor.phasePriority = 600;
}

AudioSubSystem::~AudioSubSystem() {
    shutdown();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

const ayt::game::SubSystemDescriptor& AudioSubSystem::getDescriptor() const {
    return _descriptor;
}

void AudioSubSystem::setBackend(std::unique_ptr<IAudioBackend> backend) {
    // The host installs a backend once, before initialize() runs. Once
    // initialized the engine already owns a backend; swapping here would
    // require coordinated shutdown — defer that to v2 if needed.
    if (_engine != nullptr && _engine->isInitialized()) {
        return;
    }
    _backend = std::move(backend);
}

AudioClipId AudioSubSystem::registerClipFromResource(const ayt::resource::IAudio* audio) {
    if (audio == nullptr) {
        return InvalidClipId;
    }
    if (_engine == nullptr || !_engine->isInitialized()) {
        return InvalidClipId;
    }

    AudioSampleFormat fmt = AudioSampleFormat::F32;
    if (!mapBitsPerSample(audio->getBitsPerSample(), fmt)) {
        return InvalidClipId;
    }

    const uint32_t channels = audio->getChannels();
    if (channels != 1 && channels != 2) {
        return InvalidClipId;
    }
    // Decoder/cooked metadata may count frames or interleaved samples.
    // The PCM byte length is authoritative and also bounds every read.
    const uint32_t bytesPerFrame = channels * (audio->getBitsPerSample() / 8u);
    const uint64_t dataSize = audio->getDataSize();
    if (!audio->getData() || !dataSize || dataSize % bytesPerFrame != 0u) return InvalidClipId;
    const uint64_t frameCount = dataSize / bytesPerFrame;
    if (frameCount > UINT32_MAX) return InvalidClipId;

    AudioClipDesc desc{};
    desc.sampleRate    = audio->getSampleRate();
    desc.channels      = static_cast<uint16_t>(channels);
    desc.bitsPerSample = static_cast<uint16_t>(audio->getBitsPerSample());
    desc.format        = fmt;
    desc.frameCount    = static_cast<uint32_t>(frameCount);
    desc.pcmData       = audio->getData();
    return _engine->registerClip(desc);
}

void AudioSubSystem::closePump(StreamPump& pump, bool closeEngineStream) {
    if (pump.decoder != nullptr) {
        ayt::resource::closeAudioStreamDecoder(
            static_cast<ayt::resource::AudioStreamDecoder*>(pump.decoder));
        pump.decoder = nullptr;
    }
    if (closeEngineStream && _engine && pump.streamId != InvalidStreamId) {
        _engine->closeStream(pump.streamId);
    }
    pump.streamId = InvalidStreamId;
    pump.voice = InvalidVoice;
    pump.eosMarked = false;
}

StreamingPlayback AudioSubSystem::playStreamingFile(const std::string& path,
                                                    AudioBus bus,
                                                    bool loop,
                                                    float volume) {
    StreamingPlayback out{};
    if (_engine == nullptr || !_engine->isInitialized() || path.empty()) {
        std::fprintf(stderr,
                     "[AYAudio] playStreamingFile: reject (engine=%p init=%d "
                     "pathEmpty=%d)\n",
                     static_cast<void*>(_engine.get()),
                     (_engine && _engine->isInitialized()) ? 1 : 0,
                     path.empty() ? 1 : 0);
        std::fflush(stderr);
        return out;
    }

    ayt::resource::AudioStreamDecoder* dec = ayt::resource::openAudioStreamDecoder(path);
    if (dec == nullptr) {
        std::fprintf(stderr,
                     "[AYAudio] playStreamingFile: openAudioStreamDecoder "
                     "failed path=\"%s\" (missing file / bad encode / unsupported "
                     "ext — need .wav/.mp3/.ogg)\n",
                     path.c_str());
        std::fflush(stderr);
        return out;
    }

    const uint16_t channels = ayt::resource::audioStreamDecoderChannels(dec);
    const uint32_t sourceRate = ayt::resource::audioStreamDecoderSampleRate(dec);
    if ((channels != 1 && channels != 2) || sourceRate == 0) {
        std::fprintf(stderr,
                     "[AYAudio] playStreamingFile: bad stream meta ch=%u rate=%u\n",
                     static_cast<unsigned>(channels), sourceRate);
        std::fflush(stderr);
        ayt::resource::closeAudioStreamDecoder(dec);
        return out;
    }

    AudioStreamDesc desc{};
    desc.channels = channels;
    desc.sampleRate = _engine->settings().sampleRate > 0
                          ? _engine->settings().sampleRate
                          : ayt::resource::kEngineAudioSampleRate;
    desc.capacityFrames = 16384;
    desc.loop = loop;

    const AudioStreamId streamId = _engine->openStream(desc);
    if (streamId == InvalidStreamId) {
        std::fprintf(stderr, "[AYAudio] playStreamingFile: openStream failed\n");
        std::fflush(stderr);
        ayt::resource::closeAudioStreamDecoder(dec);
        return out;
    }

    StreamPump pump{};
    pump.streamId = streamId;
    pump.decoder = dec;
    pump.path = path;
    pump.sourceRate = sourceRate;
    pump.channels = channels;
    pump.loop = loop;
    pump.decodeScratch.resize(static_cast<size_t>(2048) * channels);
    pump.pushScratch.resize(static_cast<size_t>(4096) * channels);

    // Prefill, skip near-silent lead-in (common on ambient beds), then
    // optionally makeup so quiet masters are audible without clipping later.
    _streamPumps.push_back(std::move(pump));
    float contentPeak = 0.0f;
    for (int pass = 0; pass < 64; ++pass) {
        pumpStreams();
        StreamPump& p = _streamPumps.back();
        if (p.lastPumpPeak > contentPeak) {
            contentPeak = p.lastPumpPeak;
        }
        if (contentPeak >= 0.02f || p.eosMarked) {
            break;
        }
        // Discard near-silent prefill so playback starts on real content.
        if (auto* ring = _engine->streamRing(p.streamId)) {
            float discard[2048];
            const uint32_t maxFrames =
                2048u / static_cast<uint32_t>(std::max<uint16_t>(p.channels, 1));
            while (ring->availableRead() > 0) {
                const uint32_t n = std::min(ring->availableRead(), maxFrames);
                if (n == 0) {
                    break;
                }
                ring->read(discard, n);
            }
        }
    }
    // Refill after silence skip so the ring isn't empty at playStream.
    pumpStreams();

    float playVol = volume;
    if (contentPeak > 1.0e-4f && contentPeak < 0.12f) {
        playVol = std::min(8.0f, volume * (0.30f / contentPeak));
    }

    const VoiceHandle voice =
        _engine->playStream(streamId, bus, {}, playVol, false);
    if (voice == InvalidVoice) {
        std::fprintf(stderr, "[AYAudio] playStreamingFile: playStream failed\n");
        std::fflush(stderr);
        closePump(_streamPumps.back(), true);
        _streamPumps.pop_back();
        return out;
    }
    _streamPumps.back().voice = voice;

    std::fprintf(stderr,
                 "[AYAudio] playStreamingFile: OK path=\"%s\" ch=%u srcRate=%u "
                 "engineRate=%u voice=%llu stream=%u vol=%.2f (req=%.2f) "
                 "contentPeak=%.4f bus=%u\n",
                 path.c_str(), static_cast<unsigned>(channels), sourceRate,
                 desc.sampleRate, static_cast<unsigned long long>(voice), streamId, playVol, volume, contentPeak,
                 static_cast<unsigned>(bus));
    std::fflush(stderr);

    out.stream = streamId;
    out.voice = voice;
    return out;
}

void AudioSubSystem::stopStreaming(StreamingPlayback playback) {
    if (_engine == nullptr) {
        return;
    }
    if (playback.voice != InvalidVoice) {
        _engine->stop(playback.voice);
    }
    for (auto it = _streamPumps.begin(); it != _streamPumps.end(); ++it) {
        if (it->streamId == playback.stream || it->voice == playback.voice) {
            closePump(*it, true);
            _streamPumps.erase(it);
            return;
        }
    }
    if (playback.stream != InvalidStreamId) {
        _engine->closeStream(playback.stream);
    }
}

void AudioSubSystem::pumpStreams() {
    if (_engine == nullptr || !_engine->isInitialized()) {
        return;
    }
    const uint32_t targetRate = _engine->settings().sampleRate > 0
                                    ? _engine->settings().sampleRate
                                    : ayt::resource::kEngineAudioSampleRate;

    for (auto it = _streamPumps.begin(); it != _streamPumps.end();) {
        StreamPump& pump = *it;
        if (pump.eosMarked || pump.decoder == nullptr || pump.streamId == InvalidStreamId) {
            ++it;
            continue;
        }

        auto* ring = _engine->streamRing(pump.streamId);
        if (ring == nullptr) {
            closePump(pump, false);
            it = _streamPumps.erase(it);
            continue;
        }

        // Keep the ring reasonably full (~half capacity) without spinning forever.
        int safety = 8;
        float pumpPeak = 0.0f;
        uint32_t pumpFrames = 0;
        while (safety-- > 0 && ring->availableWrite() >= 1024) {
            auto* dec = static_cast<ayt::resource::AudioStreamDecoder*>(pump.decoder);
            bool eos = false;
            const uint32_t maxDecode = 1024;
            const uint32_t got = ayt::resource::decodeAudioStreamFrames(
                dec, pump.decodeScratch.data(), maxDecode, &eos);

            if (got > 0) {
                const uint32_t outFrames = resampleFloatInterleaved(
                    pump.decodeScratch.data(), got, pump.channels,
                    pump.sourceRate, targetRate,
                    pump.pushScratch.data(),
                    static_cast<uint32_t>(pump.pushScratch.size() / pump.channels));
                if (outFrames > 0) {
                    const size_t n =
                        static_cast<size_t>(outFrames) * pump.channels;
                    for (size_t i = 0; i < n; ++i) {
                        const float a = std::fabs(pump.pushScratch[i]);
                        if (a > pumpPeak) {
                            pumpPeak = a;
                        }
                    }
                    pumpFrames += outFrames;
                    _engine->streamPush(pump.streamId, pump.pushScratch.data(),
                                        outFrames);
                }
            }

            if (eos || got == 0) {
                if (pump.loop) {
                    ayt::resource::closeAudioStreamDecoder(dec);
                    pump.decoder = ayt::resource::openAudioStreamDecoder(pump.path);
                    if (pump.decoder == nullptr) {
                        _engine->markStreamEndOfStream(pump.streamId);
                        pump.eosMarked = true;
                        break;
                    }
                    continue;
                }
                _engine->markStreamEndOfStream(pump.streamId);
                pump.eosMarked = true;
                ayt::resource::closeAudioStreamDecoder(dec);
                pump.decoder = nullptr;
                break;
            }
        }
        if (pumpFrames > 0) {
            pump.lastPumpPeak = pumpPeak;
            ++pump.diagTicks;
            // Throttled diagnostic: first fill + every ~1s @ 60 Hz hosts.
            if (pump.diagTicks == 1 || (pump.diagTicks % 60) == 0) {
                std::fprintf(stderr,
                             "[AYAudio] stream.pump id=%u frames=%u peak=%.4f "
                             "ringR=%u ringW=%u src=%u->%uHz\n",
                             pump.streamId, pumpFrames, pumpPeak,
                             ring->availableRead(), ring->availableWrite(),
                             pump.sourceRate, targetRate);
                std::fflush(stderr);
            }
        }
        ++it;
    }
}

bool AudioSubSystem::initialize() {
    if (_engine && _engine->isInitialized()) {
        return true; // idempotent (Editor Edit-mode lazy init + Play prepare)
    }
    _engine = std::make_unique<AudioEngine>();

    AudioSettings settings{};
    // AudioSettings.backend is a hint; the engine reads the actual backend
    // from the unique_ptr installed via setBackend(). We mirror the choice
    // so debug introspection (engine.settings()) reflects reality.
    settings.backend = (_backend != nullptr) ? AudioBackendKind::Miniaudio
                                            : AudioBackendKind::Null;
    settings.sampleRate = 48000;
    settings.channels = 2;
    settings.commandQueueCapacity = 256;
    settings.maxVoices = 32;
    settings.maxClips = 64;

    // Hand the host-installed backend to the engine BEFORE initialize() so
    // AudioEngine::initialize picks it up (calls backend->start + attaches
    // the engine for MiniaudioBackend).
    if (_backend != nullptr) {
        _engine->setBackend(std::move(_backend));
    }

    if (!_engine->initialize(settings)) {
        _engine.reset();
        return false;
    }

    // Hosts that use ECS should call registerAudioEmitterSystem() after
    // World::initialize (e.g. from Application bootstrap). Auto-register
    // only when World is already live so unit tests don't pollute the
    // singleton World.
    if (ayt::entity::World::instance().isInitialized()) {
        registerAudioEmitterSystem();
    }
    return true;
}

void AudioSubSystem::update(float deltaTime) {
    (void)deltaTime;
    pumpStreams();
    if (_engine) {
        _engine->submitFrame(deltaTime);
    }
}

void AudioSubSystem::fixedUpdate(float /*fixedDeltaTime*/) {
    // Audio playback is driven by the audio thread / backend callback, not by
    // fixed timestep. Nothing to do here in v0.5.
}

void AudioSubSystem::shutdown() {
    for (StreamPump& pump : _streamPumps) {
        closePump(pump, true);
    }
    _streamPumps.clear();

    if (_engine) {
        _engine->shutdown();
        _engine.reset();
    }
    // If we never handed the backend to the engine (e.g. initialize() failed
    // before that step), make sure it gets stopped and dropped here.
    _backend.reset();
}

} // namespace ayt::audio
