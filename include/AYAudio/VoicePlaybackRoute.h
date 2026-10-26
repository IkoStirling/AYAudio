#pragma once

#include <AYAudio/AudioEngine.h>
#include <AYAudio/AudioStreamRingBuffer.h>
#include <AYAudio/VoiceFrames.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ayt::audio {

/// One remote speaker routed into AudioEngine's Voice bus and spatializer.
/// The host owns AudioEngine and calls close before shutting that engine down.
/// One route occupies one of the engine's configured stream slots.
class VoicePlaybackRoute {
public:
    explicit VoicePlaybackRoute(std::uint64_t speakerId,
                                std::size_t primeFrames = 3) noexcept
        : _queue(speakerId, primeFrames) {}

    /// The source rate must equal the engine output rate; resampling or a
    /// compressed voice codec can be inserted upstream by the host.
    bool open(AudioEngine& engine, const SpatializationParams& spatial = {}) {
        if (_stream != InvalidStreamId || !engine.isInitialized()) return false;
        const auto rate = engine.settings().sampleRate;
        if (rate != 8'000 && rate != 16'000 && rate != 24'000 && rate != 48'000)
            return false;
        _sampleRate = rate;
        _spatial = spatial;
        return openStream(engine);
    }

    void close(AudioEngine& engine) {
        closeStream(engine);
        _queue.clear();
        _queue.setMuted(false);
        _sampleRate = 0;
    }

    bool submit(VoicePcmFrame frame) {
        return _stream != InvalidStreamId && frame.sampleRate == _sampleRate &&
               frame.channels == 1 && _queue.push(std::move(frame));
    }

    /// Call at most once per 20 ms output interval on the engine owner thread.
    /// Buffering leaves the stream empty; concealed loss pushes 20 ms silence.
    VoicePopStatus pump(AudioEngine& engine) {
        if (_stream == InvalidStreamId || _queue.muted())
            return VoicePopStatus::Buffering;
        const auto frames = _sampleRate / 50;
        const auto* ring = engine.streamRing(_stream);
        if (!ring || ring->availableWrite() < frames) return VoicePopStatus::Buffering;
        auto result = _queue.pop();
        if (result.status == VoicePopStatus::Buffering) return result.status;
        _scratch.resize(frames);
        for (std::size_t i = 0; i < frames; ++i)
            _scratch[i] = static_cast<float>(result.frame.samples[i]) / 32768.f;
        if (engine.streamPush(_stream, _scratch.data(), frames) != frames)
            return VoicePopStatus::Buffering;
        return result.status;
    }

    /// Close the old stream on mute so queued speech cannot play after unmute.
    /// Unmute opens a fresh stream and may fail when all slots are occupied.
    bool setMuted(AudioEngine& engine, bool muted) {
        if (_sampleRate == 0) return false;
        if (muted) {
            _queue.setMuted(true);
            closeStream(engine);
            return true;
        }
        if (!_queue.muted()) return true;
        _queue.setMuted(false);
        if (openStream(engine)) return true;
        _queue.setMuted(true);
        return false;
    }

    void setSpatial(AudioEngine& engine, const SpatializationParams& spatial) {
        _spatial = spatial;
        if (_voice != InvalidVoice) engine.setVoiceSpatial(_voice, spatial);
    }
    void setVolume(AudioEngine& engine, float volume) {
        _volume = std::clamp(volume, 0.f, 1.f);
        if (_voice != InvalidVoice) engine.setVoiceVolume(_voice, _volume);
    }
    AudioStreamId stream() const noexcept { return _stream; }
    VoiceHandle voice() const noexcept { return _voice; }
    std::size_t buffered() const noexcept { return _queue.buffered(); }

private:
    bool openStream(AudioEngine& engine) {
        AudioStreamDesc desc{};
        desc.channels = 1;
        desc.sampleRate = _sampleRate;
        desc.capacityFrames = std::max<std::uint32_t>(_sampleRate / 5, 1024);
        desc.advanceClockOnUnderrun = false;
        const auto stream = engine.openStream(desc);
        if (stream == InvalidStreamId) return false;
        const auto voice = engine.playStream(stream, AudioBus::Voice, _spatial,
                                             _volume, false);
        if (voice == InvalidVoice) {
            engine.closeStream(stream);
            return false;
        }
        _stream = stream;
        _voice = voice;
        return true;
    }
    void closeStream(AudioEngine& engine) {
        if (_voice != InvalidVoice) engine.stop(_voice);
        if (_stream != InvalidStreamId) engine.closeStream(_stream);
        _voice = InvalidVoice;
        _stream = InvalidStreamId;
    }

    VoiceJitterQueue _queue;
    std::uint32_t _sampleRate = 0;
    AudioStreamId _stream = InvalidStreamId;
    VoiceHandle _voice = InvalidVoice;
    SpatializationParams _spatial{};
    float _volume = 1.f;
    std::vector<float> _scratch;
};

} // namespace ayt::audio
