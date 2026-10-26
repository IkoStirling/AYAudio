#pragma once

#include <AYAudio/VoiceRoomRouter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>

namespace ayt::audio {

enum class VoiceIngressStatus : std::uint8_t {
    Accepted, Invalid, IdentityMismatch, RateLimited, Inaudible, RejectedFrame
};

/// Owner-thread ingress for the reference 20 ms PCM16 wire format. The host
/// authenticates a connection and supplies its speaker ID and monotonic time;
/// packet bytes cannot claim another identity. A per-speaker token bucket
/// bounds accepted attempts before decoding. This is not network authentication
/// or a packet transport. Inactive speakers do not retain ingress state.
class VoicePacketIngress {
public:
    static constexpr std::size_t kMaximumWireBytes =
        VoicePcmWire::HeaderBytes + (48'000 / 50) * 2 * 2;

    explicit VoicePacketIngress(std::uint16_t burstFrames = 8,
                                std::uint16_t framesPerSecond = 60) noexcept
        : _burst(burstFrames), _rate(framesPerSecond) {}

    /// Invalid packet/identity attempts consume an admitted speaker's rate
    /// token. An unopened speaker is dropped before decoding or state growth.
    [[nodiscard]] VoiceIngressStatus accept(VoiceRoomRouter& room,
        std::uint64_t authenticatedSpeakerId,
        std::span<const std::uint8_t> wire,
        std::uint64_t monotonicMs) {
        if (!_burst || _burst > 16 || !_rate || _rate > 100 ||
            !authenticatedSpeakerId || wire.size() < VoicePcmWire::HeaderBytes ||
            wire.size() > kMaximumWireBytes)
            return VoiceIngressStatus::Invalid;
        for (auto it = _states.begin(); it != _states.end();) {
            if (!room.audible(it->first)) it = _states.erase(it);
            else ++it;
        }
        if (!room.audible(authenticatedSpeakerId))
            return VoiceIngressStatus::Inaudible;
        auto found = _states.find(authenticatedSpeakerId);
        if (found == _states.end()) {
            if (_states.size() >= VoiceRoomRouter::kMaxAudible)
                return VoiceIngressStatus::RateLimited;
            found = _states.emplace(authenticatedSpeakerId,
                State{monotonicMs, std::uint32_t(_burst) * 1000u}).first;
        }
        auto& state = found->second;
        if (monotonicMs < state.lastMs)
            return VoiceIngressStatus::Invalid;
        const auto elapsed = std::min<std::uint64_t>(
            monotonicMs - state.lastMs, 1000);
        state.tokens = std::min<std::uint32_t>(
            std::uint32_t(_burst) * 1000u,
            state.tokens + std::uint32_t(elapsed) * _rate);
        state.lastMs = monotonicMs;
        if (state.tokens < 1000u)
            return VoiceIngressStatus::RateLimited;
        state.tokens -= 1000u;

        VoicePcmFrame frame;
        if (!VoicePcmWire::decode(wire.data(), wire.size(), frame))
            return VoiceIngressStatus::Invalid;
        if (frame.speakerId != authenticatedSpeakerId)
            return VoiceIngressStatus::IdentityMismatch;
        return room.submit(std::move(frame))
            ? VoiceIngressStatus::Accepted
            : VoiceIngressStatus::RejectedFrame;
    }

    void reset() noexcept { _states.clear(); }
    [[nodiscard]] std::size_t trackedSpeakers() const noexcept {
        return _states.size();
    }

private:
    struct State { std::uint64_t lastMs; std::uint32_t tokens; };
    std::uint16_t _burst, _rate;
    std::map<std::uint64_t, State> _states;
};

} // namespace ayt::audio
