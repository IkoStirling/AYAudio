#include <AYAudio/AudioContentPlayer.h>
#include <algorithm>
#include <cmath>
namespace ayt::audio {
AudioContentPlayer::AudioContentPlayer(AudioEngine &e)
    : _engine(&e), _generation(e.lifecycleGeneration()) {}
AudioContentPlayer::~AudioContentPlayer() { clear(); }
bool AudioContentPlayer::valid() const {
  return _engine && _engine->isInitialized() &&
         _engine->lifecycleGeneration() == _generation;
}
void AudioContentPlayer::abandon() {
  _engine = nullptr;
  _content.reset();
  _events.clear();
  _voice = 0;
  _clip = 0;
  _started = false;
  _next = 0;
  _position = 0;
}
void AudioContentPlayer::stop() {
  if (valid())
    _engine->stop(_voice);
  _voice = 0;
  _started = false;
  _next = 0;
  _position = 0;
}
void AudioContentPlayer::clear() {
  stop();
  if (valid())
    _engine->releaseClip(_clip);
  _clip = 0;
  _content.reset();
  _events.clear();
}
bool AudioContentPlayer::load(
    std::shared_ptr<const resource::AudioContent> content) {
  if (!valid() || !content || !content->isLoaded())
    return false;
  AudioClipDesc desc;
  desc.sampleRate = content->getSampleRate();
  desc.channels = uint16_t(content->getChannels());
  desc.bitsPerSample = uint16_t(content->getBitsPerSample());
  desc.format = desc.bitsPerSample == 16 ? AudioSampleFormat::S16
                                         : AudioSampleFormat::F32;
  desc.frameCount =
      content->getDataSize() / (desc.channels * (desc.bitsPerSample / 8));
  desc.pcmData = content->getData();
  auto events = content->events();
  const auto clip = _engine->registerClip(desc);
  if (!clip)
    return false;
  clear();
  _clip = clip;
  _sourceRate = desc.sampleRate;
  _sourceFrames = desc.frameCount;
  _events = std::move(events);
  _content = std::move(content);
  return true;
}
bool AudioContentPlayer::play(AudioBus bus, float gain) {
  if (!valid() || !_clip || bus >= AudioBus::Count || !std::isfinite(gain) ||
      gain < 0 || gain > 8)
    return false;
  stop();
  _voice = _engine->play(_clip, bus, {}, gain);
  _started = _voice != 0;
  return _started;
}
void AudioContentPlayer::pause(bool value) {
  if (valid())
    _engine->setVoicePaused(_voice, value);
}
bool AudioContentPlayer::seek(double seconds) {
  if (!valid() || !_content || !_voice || !std::isfinite(seconds) ||
      seconds < 0 || seconds >= double(_sourceFrames) / _sourceRate)
    return false;
  if (!_engine->setVoicePositionFrames(
          _voice,
          uint64_t(std::llround(seconds * _engine->outputSampleRate()))))
    return false;
  _position = seconds;
  const auto frame = uint64_t(std::llround(seconds * _sourceRate));
  _next = size_t(std::lower_bound(_events.begin(), _events.end(), frame,
                                  [](const auto &e, uint64_t f) {
                                    return e.beginFrame < f;
                                  }) -
                 _events.begin());
  return true;
}
bool AudioContentPlayer::playing() const {
  if (!valid())
    return false;
  VoiceRuntime state;
  return _engine->voiceState(_voice, state) && state.playing;
}
std::vector<resource::AudioContentEvent> AudioContentPlayer::update() {
  std::vector<resource::AudioContentEvent> events;
  if (!valid() || !_content || !_started)
    return events;
  VoiceRuntime state;
  const bool active = _engine->voiceState(_voice, state) && state.playing;
  if (active)
    _position = double(state.positionFrames) / _engine->outputSampleRate();
  else {
    bool found = false;
    for (uint32_t slot = 0; slot < _engine->settings().maxVoices; ++slot) {
      VoiceRuntime snapshot;
      if (_engine->voiceStateAt(slot, snapshot) && snapshot.voiceId == _voice) {
        state = snapshot;
        found = true;
        break;
      }
    }
    const double duration = double(_sourceFrames) / _sourceRate;
    if (!found || double(state.positionFrames) / _engine->outputSampleRate() <
                      duration - 1. / _engine->outputSampleRate()) {
      _started = false;
      _voice = 0;
      return events;
    }
    _position = duration;
  }
  const auto frame = uint64_t(std::llround(_position * _sourceRate));
  const auto &timeline = _events;
  while (_next < timeline.size() && timeline[_next].beginFrame <= frame &&
         events.size() < 4096)
    events.push_back(timeline[_next++]);
  if (!active && _next == timeline.size()) {
    _started = false;
    _voice = 0;
  }
  return events;
}
} // namespace ayt::audio
