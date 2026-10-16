#pragma once
#include <AYAudio/AudioEngine.h>
#include <AYResource/AudioContent.h>
namespace ayt::audio {
/// Owner-thread playback of cooked .acb from ResourceManager (loose or Pak).
/// Engine outlives this controller; destroy/clear before host shutdown, or
/// abandon after replacement. Owns only its registered clip/voice. No
/// scoring/input policy is installed.
class AudioContentPlayer {
public:
  explicit AudioContentPlayer(AudioEngine &engine);
  ~AudioContentPlayer();
  AudioContentPlayer(const AudioContentPlayer &) = delete;
  AudioContentPlayer &operator=(const AudioContentPlayer &) = delete;
  /// Copies PCM into the engine; invalid content/capacity failure preserves
  /// previous content.
  bool load(std::shared_ptr<const resource::AudioContent> content);
  bool play(AudioBus bus = AudioBus::Music, float gain = 1);
  void pause(bool paused);
  bool seek(double seconds);
  void stop();
  void clear();
  void abandon();
  /// Poll playback and return newly crossed event heads, including time zero.
  /// <=4096 events per call; overdue events remain pending until subsequent
  /// calls.
  std::vector<resource::AudioContentEvent> update();
  double positionSeconds() const { return _position; }
  bool playing() const;

private:
  bool valid() const;
  AudioEngine *_engine;
  uint64_t _generation;
  std::shared_ptr<const resource::AudioContent> _content;
  std::vector<resource::AudioContentEvent> _events;
  uint32_t _sourceRate = 48000;
  uint64_t _sourceFrames = 0;
  AudioClipId _clip = 0;
  VoiceHandle _voice = 0;
  size_t _next = 0;
  double _position = 0;
  bool _started = false;
};
} // namespace ayt::audio
