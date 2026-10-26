#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ayt::audio {

struct VoicePcmFrame {
    std::uint64_t speakerId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::vector<std::int16_t> samples;
};

/// A simple, bounded reference wire format for 20 ms PCM16 voice frames.
/// Games can replace this codec while preserving the jitter queue contract.
class VoicePcmWire {
public:
    static constexpr std::size_t HeaderBytes = 22;

    /// Encode one 20 ms interleaved block; failure leaves out unchanged.
    static bool encode(std::uint64_t speakerId, std::uint32_t sequence,
                       std::uint32_t sampleRate, std::uint8_t channels,
                       const float* pcm, std::size_t frameCount,
                       std::vector<std::uint8_t>& out) {
        const auto rateCode = code(sampleRate);
        if (!speakerId || !rateCode || (channels != 1 && channels != 2) ||
            !pcm || frameCount != sampleRate / 50) return false;
        std::vector<std::uint8_t> encoded;
        encoded.reserve(HeaderBytes + frameCount * channels * 2);
        encoded.insert(encoded.end(), {'A', 'Y', 'V', 'C', 1, channels, rateCode, 0});
        append64(encoded, speakerId);
        append32(encoded, sequence);
        append16(encoded, static_cast<std::uint16_t>(frameCount));
        for (std::size_t i = 0; i < frameCount * channels; ++i) {
            const auto finite = std::isfinite(pcm[i]) ? pcm[i] : 0.f;
            const auto scaled = static_cast<std::int16_t>(std::lround(
                std::clamp(finite, -1.f, 1.f) * 32767.f));
            append16(encoded, static_cast<std::uint16_t>(scaled));
        }
        out = std::move(encoded);
        return true;
    }

    /// Validate all header, length and format fields before replacing out.
    static bool decode(const std::uint8_t* bytes, std::size_t size,
                       VoicePcmFrame& out) {
        if (!bytes || size < HeaderBytes || bytes[0] != 'A' ||
            bytes[1] != 'Y' || bytes[2] != 'V' || bytes[3] != 'C' ||
            bytes[4] != 1 || (bytes[5] != 1 && bytes[5] != 2) ||
            bytes[7] != 0) return false;
        const auto rate = sampleRate(bytes[6]);
        const auto count = read16(bytes + 20);
        if (!rate || !read64(bytes + 8) || count != rate / 50 ||
            size != HeaderBytes + static_cast<std::size_t>(count) * bytes[5] * 2)
            return false;
        VoicePcmFrame frame;
        frame.speakerId = read64(bytes + 8);
        frame.sequence = read32(bytes + 16);
        frame.sampleRate = rate;
        frame.channels = bytes[5];
        frame.samples.reserve(static_cast<std::size_t>(count) * frame.channels);
        for (std::size_t i = 0; i < static_cast<std::size_t>(count) * frame.channels; ++i)
            frame.samples.push_back(std::bit_cast<std::int16_t>(
                read16(bytes + HeaderBytes + i * 2)));
        out = std::move(frame);
        return true;
    }

private:
    static std::uint8_t code(std::uint32_t rate) noexcept {
        switch (rate) { case 8000: return 1; case 16000: return 2;
            case 24000: return 3; case 48000: return 4; default: return 0; }
    }
    static std::uint32_t sampleRate(std::uint8_t code) noexcept {
        switch (code) { case 1: return 8000; case 2: return 16000;
            case 3: return 24000; case 4: return 48000; default: return 0; }
    }
    static void append16(std::vector<std::uint8_t>& out, std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    static void append32(std::vector<std::uint8_t>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    static void append64(std::vector<std::uint8_t>& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    static std::uint16_t read16(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
    }
    static std::uint32_t read32(const std::uint8_t* p) noexcept {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= std::uint32_t(p[i]) << (8 * i);
        return value;
    }
    static std::uint64_t read64(const std::uint8_t* p) noexcept {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= std::uint64_t(p[i]) << (8 * i);
        return value;
    }
};

enum class VoicePopStatus { Frame, Concealed, Buffering };
struct VoicePopResult { VoicePopStatus status = VoicePopStatus::Buffering; VoicePcmFrame frame; };

/// One-speaker, owner-thread jitter queue. Push authenticated, decoded frames
/// in arbitrary network order; pop at a 20 ms cadence before streamPush.
class VoiceJitterQueue {
public:
    static constexpr std::size_t MaxBuffered = 64;
    explicit VoiceJitterQueue(std::uint64_t speakerId,
                              std::size_t primeFrames = 3) noexcept
        : _speakerId(speakerId), _primeFrames(std::clamp(
              primeFrames, std::size_t{1}, std::size_t{8})) {}

    void clear() noexcept {
        _frames.clear(); _expected.reset(); _primed = false; _losses = 0;
        _sampleRate = 0; _channels = 0;
    }
    void setMuted(bool muted) noexcept {
        _muted = muted;
        if (muted) clear();
    }
    bool muted() const noexcept { return _muted; }
    std::size_t buffered() const noexcept { return _frames.size(); }

    /// Reject duplicate, stale, far-future, mismatched speaker/format frames.
    bool push(VoicePcmFrame frame) {
        if (_muted || !_speakerId || frame.speakerId != _speakerId ||
            (frame.channels != 1 && frame.channels != 2) ||
            !validRate(frame.sampleRate) ||
            frame.samples.size() != (frame.sampleRate / 50) * frame.channels)
            return false;
        if (_sampleRate && (frame.sampleRate != _sampleRate ||
                            frame.channels != _channels)) return false;
        if (!_expected) _expected = frame.sequence;
        if (!_primed) {
            const auto backward = static_cast<std::uint32_t>(*_expected - frame.sequence);
            if (backward > 0 && backward < MaxBuffered) _expected = frame.sequence;
        }
        const auto distance = static_cast<std::uint32_t>(frame.sequence - *_expected);
        if (distance >= MaxBuffered || _frames.size() >= MaxBuffered ||
            _frames.count(frame.sequence)) return false;
        _sampleRate = frame.sampleRate;
        _channels = frame.channels;
        _frames.emplace(frame.sequence, std::move(frame));
        return true;
    }

    /// Deliver one ordered block, silence for a gap, or Buffering until primed.
    VoicePopResult pop() {
        if (_muted || !_expected) return {};
        if (!_primed) {
            if (_frames.size() < _primeFrames) return {};
            _primed = true;
        }
        const auto it = _frames.find(*_expected);
        if (it != _frames.end()) {
            VoicePcmFrame result = std::move(it->second);
            _frames.erase(it);
            ++*_expected;
            _losses = 0;
            return {VoicePopStatus::Frame, std::move(result)};
        }
        if (_frames.empty() && _losses >= 2) {
            clear();
            return {};
        }
        VoicePcmFrame silence;
        silence.speakerId = _speakerId;
        silence.sequence = (*_expected)++;
        silence.sampleRate = _sampleRate;
        silence.channels = _channels;
        silence.samples.resize((_sampleRate / 50) * _channels, 0);
        ++_losses;
        return {VoicePopStatus::Concealed, std::move(silence)};
    }

private:
    static bool validRate(std::uint32_t rate) noexcept {
        return rate == 8000 || rate == 16000 || rate == 24000 || rate == 48000;
    }
    std::uint64_t _speakerId;
    std::size_t _primeFrames;
    std::map<std::uint32_t, VoicePcmFrame> _frames;
    std::optional<std::uint32_t> _expected;
    std::uint32_t _sampleRate = 0;
    std::uint8_t _channels = 0;
    unsigned _losses = 0;
    bool _primed = false;
    bool _muted = false;
};

} // namespace ayt::audio
