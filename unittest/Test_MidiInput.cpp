#include "AYTest.h"
#include <AYAudio/MidiInput.h>
#include <cmath>
#include <thread>
using namespace ayt::audio;
TEST_SUITE(AudioMidiTests)
TEST_CASE(DecodeVelocityZeroAndChannelControllersRejectSystemData) {
  MidiEvent e;
  CHECK(MidiInput::decode(0x00643c92, 1, e));
  CHECK(e.kind == MidiEvent::Kind::NoteOn);
  CHECK(e.channel == 2);
  CHECK(e.key == 60);
  CHECK(e.value == 100);
  CHECK(MidiInput::decode(0x00003c92, 2, e));
  CHECK(e.kind == MidiEvent::Kind::NoteOff);
  CHECK(MidiInput::decode(0x007f40b2, 3, e));
  CHECK(e.kind == MidiEvent::Kind::ControlChange);
  CHECK(e.key == 64);
  CHECK(e.value == 127);
  CHECK(!MidiInput::decode(0xf8, 0, e));
  CHECK(!MidiInput::decode(0x00ff3c90, 0, e));
}
TEST_CASE(CallbackQueuePreservesOrderReportsOverflowAndCloseDiscardsPending) {
  MidiInput input;
  for (int i = 0; i < 1050; ++i)
    MidiInputTestAccess::receive(input, 0x00643c90, double(i));
  CHECK(input.droppedEvents() == 26);
  MidiEvent e;
  for (int i = 0; i < 1024; ++i) {
    CHECK(input.poll(e));
    CHECK(e.time == double(i));
  }
  CHECK(!input.poll(e));
  MidiInputTestAccess::receive(input, 0x00003c80, 0);
  input.close();
  CHECK(!input.poll(e));
  CHECK(input.droppedEvents() == 0);
}
TEST_CASE(CallbackAndOwnerExchangeIndependentEvents) {
  MidiInput input;
  std::thread producer([&] {
    for (int i = 0; i < 100000; ++i)
      MidiInputTestAccess::receive(input, 0x00643c90, double(i));
  });
  MidiEvent e;
  double previous = -1;
  int read = 0;
  bool ordered = true;
  for (int i = 0; i < 100000; ++i)
    if (input.poll(e)) {
      ordered &= e.time > previous;
      previous = e.time;
      ++read;
    }
  producer.join();
  while (input.poll(e)) {
    ordered &= e.time > previous;
    previous = e.time;
    ++read;
  }
  CHECK(ordered);
  CHECK(uint64_t(read) + input.droppedEvents() == 100000);
}
TEST_CASE(CalibrationFindsKnownDelayWithNoiseDcAndPolarityInversion) {
  std::vector<float> probe(256), capture(4096, .1f);
  uint32_t random = 123;
  for (auto &v : probe) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    v = float(int(random % 2001) - 1000) / 1000;
  }
  for (size_t i = 0; i < probe.size(); ++i)
    capture[713 + i] -= probe[i] * .4f;
  AudioLatencyEstimate estimate;
  std::string error;
  CHECK(estimateAudioLatency(probe, capture, 48000, 0, 3000, estimate, &error));
  CHECK_FLOAT_EQ(estimate.seconds, 713. / 48000, 1e-9);
  CHECK(estimate.confidence > .99);
  std::fill(capture.begin(), capture.end(), 0);
  CHECK(!estimateAudioLatency(probe, capture, 48000, 0, 3000, estimate));
  CHECK_FLOAT_EQ(estimate.seconds, 713. / 48000, 1e-9);
  std::copy(probe.begin(), probe.end(), capture.begin() + 300);
  std::copy(probe.begin(), probe.end(), capture.begin() + 2000);
  CHECK(!estimateAudioLatency(probe, capture, 48000, 0, 3000, estimate));
}
TEST_SUITE_END
