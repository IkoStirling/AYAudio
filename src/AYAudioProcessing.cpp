#include <AYAudio/AudioProcessing.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <mysofa.h>

namespace ayt::audio {
namespace {
constexpr double Pi = 3.14159265358979323846;
bool fail(std::string *e, const char *message) {
  if (e)
    *e = message;
  return false;
}
} // namespace
bool AudioEffectChain::configure(const std::vector<AudioEffect> &effects,
                                 uint32_t rate, std::string *error) {
  if (rate < 8000 || rate > 192000 || effects.size() > 8)
    return fail(error, "invalid effect rate or chain size");
  std::vector<State> states(effects.size());
  for (size_t i = 0; i < effects.size(); ++i) {
    const auto &e = effects[i];
    auto &s = states[i];
    for (float value : {e.frequency, e.q, e.gainDb, e.thresholdDb, e.ratio,
                        e.attack, e.release, e.delay, e.feedback, e.wet})
      if (!std::isfinite(value))
        return fail(error, "non-finite effect parameter");
    if (e.kind < AudioEffectKind::LowPass ||
        e.kind > AudioEffectKind::Limiter || e.frequency <= 0 ||
        e.frequency >= rate * .5f || e.q < .1f || e.q > 20 ||
        std::abs(e.gainDb) > 36 || e.thresholdDb > 0 || e.thresholdDb < -96 ||
        e.ratio < 1 || e.ratio > 100 || e.attack < .0001f || e.attack > 10 ||
        e.release < .0001f || e.release > 10 || e.delay < 0 || e.delay > 2 ||
        std::abs(e.feedback) >= .99f || e.wet < 0 || e.wet > 1)
      return fail(error, "effect parameter out of range");
    const double w = 2 * Pi * e.frequency / rate, c = std::cos(w),
                 a = std::sin(w) / (2 * e.q), A = std::pow(10., e.gainDb / 40);
    if (e.kind == AudioEffectKind::LowPass)
      s.coefficients = {(1 - c) / 2, 1 - c, (1 - c) / 2, -2 * c, 1 - a};
    if (e.kind == AudioEffectKind::HighPass)
      s.coefficients = {(1 + c) / 2, -(1 + c), (1 + c) / 2, -2 * c, 1 - a};
    if (e.kind == AudioEffectKind::LowPass ||
        e.kind == AudioEffectKind::HighPass)
      for (auto &v : s.coefficients)
        v /= 1 + a;
    if (e.kind == AudioEffectKind::PeakEq) {
      s.coefficients = {1 + a * A, -2 * c, 1 - a * A, -2 * c, 1 - a / A};
      for (auto &v : s.coefficients)
        v /= 1 + a / A;
    }
    s.attack = std::exp(-1.f / (e.attack * rate));
    s.release = std::exp(-1.f / (e.release * rate));
    if (e.kind == AudioEffectKind::Delay)
      s.delay.assign(
          std::max<size_t>(1, size_t(std::llround(e.delay * rate))) * 2, 0);
  }
  _rate = rate;
  _effects = effects;
  _states = std::move(states);
  if (error)
    error->clear();
  return true;
}
void AudioEffectChain::reset() noexcept {
  for (auto &s : _states) {
    s.z1 = {};
    s.z2 = {};
    s.envelope = 0;
    s.cursor = 0;
    std::fill(s.delay.begin(), s.delay.end(), 0);
  }
}
void AudioEffectChain::process(float *data, uint32_t frames,
                               uint16_t channels) noexcept {
  if (!data || (channels != 1 && channels != 2))
    return;
  for (size_t i = 0; i < _effects.size(); ++i) {
    const auto &e = _effects[i];
    auto &s = _states[i];
    for (uint32_t f = 0; f < frames; ++f) {
      float gain = 1;
      if (e.kind == AudioEffectKind::Compressor ||
          e.kind == AudioEffectKind::Limiter) {
        float peak = std::abs(data[f * channels]);
        if (channels == 2)
          peak = std::max(peak, std::abs(data[f * channels + 1]));
        const float coeff = peak > s.envelope ? s.attack : s.release;
        s.envelope = coeff * s.envelope + (1 - coeff) * peak;
        const float db = 20 * std::log10(std::max(1e-9f, s.envelope));
        if (db > e.thresholdDb)
          gain = std::pow(
              10.f, (e.thresholdDb + (db - e.thresholdDb) / e.ratio - db) / 20);
      }
      for (uint16_t ch = 0; ch < channels; ++ch) {
        float &v = data[f * channels + ch];
        switch (e.kind) {
        case AudioEffectKind::LowPass:
        case AudioEffectKind::HighPass:
        case AudioEffectKind::PeakEq: {
          const auto &b = s.coefficients;
          const double y = b[0] * v + s.z1[ch];
          s.z1[ch] = b[1] * v - b[3] * y + s.z2[ch];
          s.z2[ch] = b[2] * v - b[4] * y;
          v = float(y);
          break;
        }
        case AudioEffectKind::Delay: {
          const size_t index = s.cursor * 2 + ch;
          const float delayed = s.delay[index];
          s.delay[index] = v + delayed * e.feedback;
          v = v * (1 - e.wet) + delayed * e.wet;
          break;
        }
        case AudioEffectKind::Compressor:
          v *= gain;
          break;
        case AudioEffectKind::Limiter: {
          const float ceiling = std::pow(10.f, e.thresholdDb / 20);
          v = std::clamp(v * gain, -ceiling, ceiling);
          break;
        }
        }
        if (!std::isfinite(v))
          v = 0;
      }
      if (e.kind == AudioEffectKind::Delay)
        s.cursor = (s.cursor + 1) % (s.delay.size() / 2);
    }
  }
}
struct HrtfProfile::Impl {
  MYSOFA_EASY *easy = nullptr;
  int taps = 0;
  uint32_t rate = 0;
  mutable std::mutex mutex;
  ~Impl() {
    if (easy)
      mysofa_close(easy);
  }
};
HrtfProfile::HrtfProfile() : _impl(std::make_unique<Impl>()) {}
HrtfProfile::~HrtfProfile() = default;
uint32_t HrtfProfile::sampleRate() const { return _impl->rate; }
bool HrtfProfile::loadSofa(const std::string &path, uint32_t rate,
                           std::string *error) {
  if (rate < 8000 || rate > 192000)
    return fail(error, "SOFA sample rate out of range");
  try {
    const auto file = std::filesystem::u8path(path);
    const auto size = std::filesystem::file_size(file);
    if (!size || size > 64 * 1024 * 1024)
      return fail(error, "SOFA exceeds 64 MiB");
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (!in.read(bytes.data(), bytes.size()))
      return fail(error, "cannot read SOFA");
    auto next = std::make_unique<Impl>();
    int err = 0;
    next->easy = mysofa_open_data(bytes.data(), long(bytes.size()), float(rate),
                                  &next->taps, &err);
    if (!next->easy || next->taps <= 0 ||
        next->taps > int(BinauralFilter::MaxTaps))
      return fail(error, "unsupported SOFA or HRIR longer than 1024 taps");
    next->rate = rate;
    _impl = std::move(next);
    if (error)
      error->clear();
    return true;
  } catch (const std::exception &e) {
    if (error)
      *error = e.what();
    return false;
  }
}
bool HrtfProfile::filter(const ayt::math::FVector3 &p,
                         BinauralFilter &out) const {
  if (!_impl->easy || !std::isfinite(p.x) || !std::isfinite(p.y) ||
      !std::isfinite(p.z))
    return false;
  std::lock_guard lock(_impl->mutex);
  BinauralFilter next;
  next.taps = _impl->taps;
  float dl = 0, dr = 0;
  const float length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z),
              scale = length > 1e-6f ? 1 / length : 0;
  mysofa_getfilter_float(_impl->easy, length > 1e-6f ? -p.z * scale : 1,
                         -p.x * scale, p.y * scale, next.left.data(),
                         next.right.data(), &dl, &dr);
  next.delayLeft = dl * _impl->rate;
  next.delayRight = dr * _impl->rate;
  if (!std::isfinite(next.delayLeft) || !std::isfinite(next.delayRight) ||
      next.delayLeft < 0 || next.delayRight < 0 ||
      next.delayLeft >= BinauralFilter::MaxDelay - 1 ||
      next.delayRight >= BinauralFilter::MaxDelay - 1)
    return false;
  for (size_t i = 0; i < next.taps; ++i)
    if (!std::isfinite(next.left[i]) || !std::isfinite(next.right[i]))
      return false;
  out = next;
  return true;
}
void AudioVoiceProcessor::prepare(uint64_t identity,
                                  const BinauralFilter *filter, float cutoff,
                                  uint32_t rate, float blend, float spatialGain,
                                  float dryLeft, float dryRight) {
  if (identity != _identity) {
    _history.fill(0);
    _low = {};
    _cursor = 0;
    _tail = 0;
    _identity = identity;
  }
  bool safe = filter && filter->taps <= BinauralFilter::MaxTaps &&
              std::isfinite(filter->delayLeft) &&
              std::isfinite(filter->delayRight) && filter->delayLeft >= 0 &&
              filter->delayRight >= 0 &&
              filter->delayLeft < BinauralFilter::MaxDelay - 1 &&
              filter->delayRight < BinauralFilter::MaxDelay - 1;
  if (safe)
    for (size_t i = 0; i < filter->taps; ++i)
      if (!std::isfinite(filter->left[i]) || !std::isfinite(filter->right[i])) {
        safe = false;
        break;
      }
  _filter = safe ? *filter : BinauralFilter{};
  _alpha = std::isfinite(cutoff) && rate >= 8000 && rate <= 192000 &&
                   cutoff > 0 && cutoff < rate * .49f
               ? float(-std::expm1(-2 * Pi * std::max(20.f, cutoff) / rate))
               : 1;
  _blend = std::clamp(blend, 0.f, 1.f);
  _spatialGain = spatialGain;
  _dryLeft = dryLeft;
  _dryRight = dryRight;
}
void AudioVoiceProcessor::process(float *data, uint32_t frames) noexcept {
  for (uint32_t f = 0; f < frames; ++f) {
    const float dry[2] = {data[2 * f], data[2 * f + 1]};
    if (std::max(std::abs(dry[0]), std::abs(dry[1])) > 1e-8f)
      _tail =
          _filter.taps +
          uint32_t(std::ceil(std::max(_filter.delayLeft, _filter.delayRight))) +
          (_alpha < 1 ? uint32_t(std::min(32768.f, std::ceil(18.42f / _alpha)))
                      : 0);
    else if (_tail)
      --_tail;
    for (int ch = 0; ch < 2; ++ch) {
      _low[ch] += _alpha * (data[2 * f + ch] - _low[ch]);
      data[2 * f + ch] = _low[ch];
    }
    if (!_filter.taps)
      continue;
    _history[_cursor] = (data[2 * f] + data[2 * f + 1]) * .5f;
    for (int ch = 0; ch < 2; ++ch) {
      const auto &taps = ch ? _filter.right : _filter.left;
      const float delay = ch ? _filter.delayRight : _filter.delayLeft;
      const size_t whole = size_t(delay);
      const float frac = delay - float(whole);
      float value = 0;
      for (size_t tap = 0; tap < _filter.taps; ++tap) {
        const size_t a = (_cursor + _history.size() - whole - tap) %
                         _history.size(),
                     b = (a + _history.size() - 1) % _history.size();
        value += taps[tap] * (_history[a] * (1 - frac) + _history[b] * frac);
      }
      data[2 * f + ch] = value * _spatialGain * _blend +
                         dry[ch] * (ch ? _dryRight : _dryLeft) * (1 - _blend);
    }
    _cursor = (_cursor + 1) % _history.size();
  }
}
} // namespace ayt::audio
