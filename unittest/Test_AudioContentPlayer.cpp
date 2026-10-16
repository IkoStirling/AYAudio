#include <AYAudio/AudioContentPlayer.h>
#include <AYTest.h>
using namespace ayt::audio;
namespace {
std::shared_ptr<ayt::resource::AudioContent> content() {
  ayt::resource::Audio a;
  a.setFormat(48000, 1, 32, 4800);
  std::vector<float> pcm(4800, .1f);
  a.setData(reinterpret_cast<const uint8_t *>(pcm.data()), pcm.size() * 4);
  a.setLoaded(true);
  auto result = std::make_shared<ayt::resource::AudioContent>();
  if (!result->assign(
          a, ayt::resource::AudioContent::Kind::Chart, 120, 0,
          {{0, 100, 0, 0, 1}, {1000, 1500, 1, 0, 1}, {4700, 4700, 2, 0, 1}}))
    throw std::runtime_error("invalid fixture");
  return result;
}
} // namespace
TEST_SUITE(AudioContentPlayerTests)
TEST_CASE(
    ContentEventsTrackPlaybackSeekAndNaturalEndAndDoNotStopForeignVoices) {
  AudioEngine engine;
  CHECK(engine.initialize({}));
  AudioContentPlayer player(engine);
  auto data = content();
  CHECK(player.load(data));
  auto dataCopy = *data;
  data->unload();
  CHECK(player.play());
  const auto start = player.update();
  CHECK(start.size() == 1);
  CHECK(player.update().empty());
  *data = dataCopy;
  float output[2400]{};
  engine.render(output, 1200);
  CHECK(player.update().size() == 1);
  CHECK(player.seek(.09));
  CHECK(player.update().empty());
  engine.render(output, 1200);
  const auto tail = player.update();
  CHECK(tail.size() == 1);
  CHECK(!player.playing());
  CHECK(player.play());
  engine.stopAll();
  CHECK(player.update().empty());
  CHECK(!player.playing());
  AudioClipDesc desc;
  desc.channels = 1;
  desc.frameCount = 4800;
  desc.pcmData = data->getData();
  const auto foreign = engine.play(engine.registerClip(desc));
  player.clear();
  VoiceRuntime voice;
  CHECK(engine.voiceState(foreign, voice));
}
TEST_SUITE_END
