#pragma once

#include <AYAudio/VoicePlaybackRoute.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <vector>

namespace ayt::audio {

/// Game-supplied audibility and presentation for one authenticated speaker.
/// Higher priority wins, then nearer distance, then smaller stable speaker ID.
struct VoiceRoomCandidate {
    std::uint64_t speakerId = 0;
    std::int32_t priority = 0;
    float distanceMeters = 0.0f;
    bool muted = false;
    SpatializationParams spatial{};
    float volume = 1.0f;
};

enum class VoiceRoomUpdateStatus : std::uint8_t {
    Ok, Invalid, Partial
};
struct VoiceRoomUpdate {
    VoiceRoomUpdateStatus status = VoiceRoomUpdateStatus::Invalid;
    std::size_t selected = 0, active = 0, unavailable = 0;
};

/// Select a bounded set of remote speakers for actual playback. Inaudible
/// speakers own no stream or buffered speech; re-entry starts a fresh jitter
/// queue. The host still authenticates speakers, chooses distance/priority,
/// decodes packets and updates this router on the AudioEngine owner thread.
/// Call closeAll before AudioEngine shutdown. Open failures are reported as
/// Partial and leave the successfully opened subset active.
class VoiceRoomRouter {
public:
    static constexpr std::size_t kMaxCandidates = 1024;
    static constexpr std::size_t kMaxAudible = 64;

    explicit VoiceRoomRouter(std::size_t audibleBudget,
                             std::size_t primeFrames = 3) noexcept
        : _budget(audibleBudget), _primeFrames(primeFrames) {}

    [[nodiscard]] VoiceRoomUpdate update(AudioEngine& engine,
        std::span<const VoiceRoomCandidate> candidates) {
        VoiceRoomUpdate result;
        if (!_budget || _budget > kMaxAudible || !_primeFrames ||
            _primeFrames > 64 || candidates.size() > kMaxCandidates ||
            !engine.isInitialized()) return result;
        std::set<std::uint64_t> seen;
        std::vector<VoiceRoomCandidate> ranked;
        ranked.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (!candidate.speakerId || !seen.insert(candidate.speakerId).second ||
                !std::isfinite(candidate.distanceMeters) ||
                candidate.distanceMeters < 0.0f ||
                !std::isfinite(candidate.volume) ||
                candidate.volume < 0.0f || candidate.volume > 1.0f)
                return result;
            if (!candidate.muted) ranked.push_back(candidate);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            if (a.distanceMeters != b.distanceMeters)
                return a.distanceMeters < b.distanceMeters;
            return a.speakerId < b.speakerId;
        });
        if (ranked.size() > _budget) ranked.resize(_budget);
        std::set<std::uint64_t> selected;
        for (const auto& candidate : ranked) selected.insert(candidate.speakerId);
        result.status = VoiceRoomUpdateStatus::Ok;
        result.selected = ranked.size();

        for (auto it = _routes.begin(); it != _routes.end();) {
            if (selected.contains(it->first)) { ++it; continue; }
            it->second->close(engine);
            it = _routes.erase(it);
        }
        for (const auto& candidate : ranked) {
            auto found = _routes.find(candidate.speakerId);
            if (found == _routes.end()) {
                auto [it, inserted] = _routes.emplace(candidate.speakerId,
                    std::make_unique<VoicePlaybackRoute>(candidate.speakerId,
                        _primeFrames));
                (void)inserted;
                if (!it->second->open(engine, candidate.spatial)) {
                    _routes.erase(it);
                    ++result.unavailable;
                    continue;
                }
                found = it;
            }
            found->second->setSpatial(engine, candidate.spatial);
            found->second->setVolume(engine, candidate.volume);
        }
        result.active = _routes.size();
        if (result.unavailable) result.status = VoiceRoomUpdateStatus::Partial;
        return result;
    }

    /// Only selected, opened speakers accept frames; virtualized frames are
    /// intentionally dropped instead of accumulating delayed speech.
    [[nodiscard]] bool submit(VoicePcmFrame frame) {
        const auto found = _routes.find(frame.speakerId);
        return found != _routes.end() && found->second->submit(std::move(frame));
    }

    /// Pump each active route once per 20 ms interval on the owner thread.
    void pump(AudioEngine& engine) {
        for (auto& [id, route] : _routes) {
            (void)id;
            (void)route->pump(engine);
        }
    }

    void closeAll(AudioEngine& engine) {
        for (auto& [id, route] : _routes) {
            (void)id;
            route->close(engine);
        }
        _routes.clear();
    }
    [[nodiscard]] bool audible(std::uint64_t speakerId) const noexcept {
        return _routes.contains(speakerId);
    }
    [[nodiscard]] std::size_t activeCount() const noexcept { return _routes.size(); }

private:
    std::size_t _budget = 0, _primeFrames = 3;
    std::map<std::uint64_t, std::unique_ptr<VoicePlaybackRoute>> _routes;
};

} // namespace ayt::audio
