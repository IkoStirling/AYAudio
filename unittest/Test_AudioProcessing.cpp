#include "AYTest.h"
#include <AYAudio.h>
#include <cmath>
#include <cstdlib>
#include <limits>
using namespace ayt::audio;
TEST_SUITE(AudioProcessingTests)
TEST_CASE(
    BinauralImpulseIncludesFractionalReceiverDelayAcrossBlocksAndResetsOnReuse) {
  AudioVoiceProcessor processor;
  BinauralFilter filter;
  filter.taps = 2;
  filter.left[0] = 1;
  filter.right[1] = .5f;
  filter.delayRight = .5f;
  processor.prepare(1, &filter, 0, 48000);
  float first[2] = {1, 1};
  processor.process(first, 1);
  CHECK(first[0] == 1);
  CHECK(first[1] == 0);
  float tail[4] = {};
  processor.process(tail, 2);
  CHECK_FLOAT_EQ(tail[1], .25f, 1e-6f);
  CHECK_FLOAT_EQ(tail[3], .25f, 1e-6f);
  processor.prepare(2, &filter, 0, 48000);
  float silence[4] = {};
  processor.process(silence, 2);
  for (float v : silence)
    CHECK(v == 0);
  processor.prepare(3, &filter, 0, 48000, .25f, .5f, 1, 1);
  float blended[2] = {1, 1};
  processor.process(blended, 1);
  CHECK_FLOAT_EQ(blended[0], .875f, 1e-6f);
  CHECK_FLOAT_EQ(blended[1], .75f, 1e-6f);
}
TEST_CASE(OcclusionLowpassRejectsHighFrequenciesAndPreservesBlockContinuity) {
  AudioVoiceProcessor whole, split;
  whole.prepare(1, nullptr, 800, 48000);
  split.prepare(1, nullptr, 800, 48000);
  std::vector<float> a(1024), b;
  for (size_t i = 0; i < 512; ++i)
    a[2 * i] = a[2 * i + 1] = i % 2 ? 1.f : -1.f;
  b = a;
  whole.process(a.data(), 512);
  split.process(b.data(), 17);
  split.process(b.data() + 34, 495);
  CHECK(std::abs(a.back()) < .1f);
  float difference = 0;
  for (size_t i = 0; i < a.size(); ++i)
    difference = std::max(difference, std::abs(a[i] - b[i]));
  CHECK(difference < 1e-6f);
}
TEST_CASE(EffectDelayIsStereoAndResetClearsTailAndInvalidConfigPreservesChain) {
  AudioEffectChain chain;
  AudioEffect delay;
  delay.kind = AudioEffectKind::Delay;
  delay.delay = 2.f / 48000;
  delay.wet = 1;
  delay.feedback = 0;
  CHECK(chain.configure({delay}, 48000));
  float values[8] = {1, .5f, 0, 0, 0, 0, 0, 0};
  chain.process(values, 4, 2);
  CHECK(values[0] == 0);
  CHECK(values[4] == 1);
  CHECK(values[5] == .5f);
  delay.feedback = 1;
  CHECK(!chain.configure({delay}, 48000));
  CHECK(chain.configuration()[0].feedback == 0);
  chain.reset();
  float tail[8] = {};
  chain.process(tail, 4, 2);
  for (float v : tail)
    CHECK(v == 0);
}
TEST_CASE(OrderedFiltersEqCompressorLimiterAndEngineBusIntegration) {
  AudioEffect low;
  low.frequency = 600;
  AudioEffect high = low;
  high.kind = AudioEffectKind::HighPass;
  AudioEffect eq;
  eq.kind = AudioEffectKind::PeakEq;
  eq.gainDb = 6;
  AudioEffect compressor;
  compressor.kind = AudioEffectKind::Compressor;
  compressor.thresholdDb = -12;
  compressor.attack = .0001f;
  AudioEffect limiter;
  limiter.kind = AudioEffectKind::Limiter;
  limiter.thresholdDb = -6;
  AudioEffectChain chain;
  CHECK(chain.configure({low, eq, compressor, limiter}, 48000));
  std::vector<float> buffer(4096, 4);
  chain.process(buffer.data(), 2048, 2);
  bool finite = true;
  float peak = 0;
  for (float v : buffer) {
    finite &= std::isfinite(v);
    peak = std::max(peak, std::abs(v));
  }
  CHECK(finite);
  CHECK(peak <= .502f);
  CHECK(chain.configure({high}, 48000));
  std::fill(buffer.begin(), buffer.end(), 1);
  chain.process(buffer.data(), 2048, 2);
  CHECK(std::abs(buffer.back()) < 1e-5f);
  AudioEngine engine;
  CHECK(engine.initialize({}));
  CHECK(engine.setBusEffects(AudioBus::Sfx, {limiter}));
  std::vector<float> pcm(1024, 2);
  AudioClipDesc desc;
  desc.channels = 1;
  desc.frameCount = 1024;
  desc.pcmData = pcm.data();
  CHECK(engine.play(engine.registerClip(desc)) != InvalidVoice);
  std::vector<float> output(2048);
  engine.render(output.data(), 1024);
  CHECK(engine.outputLevels().left <= .502f);
  limiter.thresholdDb = std::numeric_limits<float>::quiet_NaN();
  CHECK(!engine.setBusEffects(AudioBus::Sfx, {limiter}));
  CHECK(engine.busEffects(AudioBus::Sfx)[0].thresholdDb == -6);
  engine.setBusGain(AudioBus::Preview, .25f);
  const auto snapshot = engine.debugSnapshot();
  CHECK(snapshot.busGains.size() == 5);
  CHECK(snapshot.allBusGains.size() == size_t(AudioBus::Count));
  CHECK(snapshot.allBusGains[size_t(AudioBus::Preview)] == .25f);
}
TEST_CASE(SofaProfileLoadsDirectionalResponsesAndRejectsInvalidReplacement) {
  const char *file = std::getenv("AY_AUDIO_SOFA_TEST_FILE");
  if (!file)
    SKIP_TEST("Set AY_AUDIO_SOFA_TEST_FILE to a licensed SimpleFreeFieldHRIR "
              "SOFA fixture");
  auto profile = std::make_shared<HrtfProfile>();
  std::string error;
  CHECK(profile->loadSofa(file, 48000, &error));
  CHECK(error.empty());
  BinauralFilter right, left;
  CHECK(profile->filter({1, 0, 0}, right));
  CHECK(profile->filter({-1, 0, 0}, left));
  CHECK(right.taps > 0);
  double difference = 0;
  for (size_t i = 0; i < right.taps; ++i)
    difference += std::abs(right.left[i] - left.left[i]);
  CHECK(difference > .01);
  CHECK(!profile->loadSofa("missing.sofa", 48000, &error));
  CHECK(profile->sampleRate() == 48000);
  AudioEngine engine;
  CHECK(engine.initialize({}));
  CHECK(engine.setHrtfProfile(profile));
  ListenerParams listener;
  listener.hrtfEnabled = true;
  engine.setListener(listener);
  std::vector<float> pcm(2048);
  pcm[0] = .2f;
  AudioClipDesc desc;
  desc.channels = 1;
  desc.pcmData = pcm.data();
  desc.frameCount = 2048;
  SpatializationParams spatial;
  spatial.mode = SpatializationParams::Mode::World;
  spatial.position = {1, 0, 0};
  CHECK(engine.play(engine.registerClip(desc), AudioBus::Sfx, spatial));
  engine.submitFrame(0);
  std::vector<float> output(4096);
  engine.render(output.data(), 2048);
  double lr = 0, energy = 0;
  for (size_t i = 0; i < 2048; ++i) {
    lr += std::abs(output[2 * i] - output[2 * i + 1]);
    energy += std::abs(output[2 * i]) + std::abs(output[2 * i + 1]);
  }
  CHECK(energy > 0);
  CHECK(lr > 0);
}
TEST_SUITE_END
