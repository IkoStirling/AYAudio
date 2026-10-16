#pragma once
#include <AYAudio/AudioTypes.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ayt::audio {
enum class AudioEffectKind {
  LowPass,
  HighPass,
  PeakEq,
  Compressor,
  Delay,
  Limiter
};
/// Ordered bus effect. Frequency in Hz, delay/attack/release in seconds, gains
/// in dB.
struct AudioEffect {
  AudioEffectKind kind = AudioEffectKind::LowPass;
  float frequency = 1000, q = .7071f, gainDb = 0;
  float thresholdDb = -12, ratio = 4, attack = .01f, release = .1f;
  float delay = .15f, feedback = .25f, wet = .3f;
};
/// Owner-thread configure, callback-thread process under the engine state gate.
/// At most eight effects, two seconds of delay each. Failed configure preserves
/// state.
class AudioEffectChain {
public:
  bool configure(const std::vector<AudioEffect> &effects, uint32_t rate,
                 std::string *error = nullptr);
  void process(float *interleaved, uint32_t frames, uint16_t channels) noexcept;
  void reset() noexcept;
  const std::vector<AudioEffect> &configuration() const { return _effects; }

private:
  struct State {
    std::array<double, 5> coefficients{1, 0, 0, 0, 0};
    std::array<double, 2> z1{}, z2{};
    std::vector<float> delay;
    size_t cursor = 0;
    float envelope = 0, attack = 0, release = 0;
  };
  uint32_t _rate = 48000;
  std::vector<AudioEffect> _effects;
  std::vector<State> _states;
};
/// A prepared directional HRIR, including SOFA receiver delays; no callback
/// allocation.
struct BinauralFilter {
  static constexpr size_t MaxTaps = 1024, MaxDelay = 2048;
  std::array<float, MaxTaps> left{}, right{};
  uint32_t taps = 0;
  float delayLeft = 0, delayRight = 0; // target-rate sample frames
};
/// Loads AES SOFA SimpleFreeFieldHRIR using libmysofa. Load/query on owner
/// thread. Returned filters are independent values. Supports 8..192 kHz, <=1024
/// taps and <2047 frames delay.
class HrtfProfile {
public:
  HrtfProfile();
  ~HrtfProfile();
  HrtfProfile(const HrtfProfile &) = delete;
  HrtfProfile &operator=(const HrtfProfile &) = delete;
  /// Load/copy/resample on the owner thread. Invalid replacement preserves the
  /// old profile.
  bool loadSofa(const std::string &utf8Path, uint32_t rate,
                std::string *error = nullptr);
  /// Engine-local x=right,y=up,z=back; converts to SOFA x=front,y=left,z=up.
  bool filter(const ayt::math::FVector3 &local, BinauralFilter &out) const;
  uint32_t sampleRate() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};
/// Internal voice DSP state. Engine preallocates one per slot; identities reset
/// history.
class AudioVoiceProcessor {
public:
  void prepare(uint64_t identity, const BinauralFilter *filter, float cutoffHz,
               uint32_t rate, float blend = 1, float spatialGain = 1,
               float dryLeft = 1, float dryRight = 1);
  void process(float *stereo, uint32_t frames) noexcept;
  bool binaural() const { return _filter.taps != 0; }
  bool hasTail() const { return _tail != 0; }

private:
  uint64_t _identity = 0;
  BinauralFilter _filter{};
  std::array<float, BinauralFilter::MaxTaps + BinauralFilter::MaxDelay>
      _history{};
  size_t _cursor = 0;
  uint32_t _tail = 0;
  float _alpha = 1;
  float _blend = 1, _spatialGain = 1, _dryLeft = 1, _dryRight = 1;
  std::array<float, 2> _low{};
};
} // namespace ayt::audio
