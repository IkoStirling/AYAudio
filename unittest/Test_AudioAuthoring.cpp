#include "../demo/audio_editor/AudioAuthoringSettings.h"
#include "../demo/audio_editor/AudioEditorSession.h"
#include <AYTest.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/TabControl.h>
#include <AYUI/TextInput.h>
#include <AYUI/Theme.h>
#include <filesystem>
#include <fstream>
using namespace ayt::audio;
namespace {
struct WorkbenchFixture {
  AudioSubSystem audio;
  ayt::ui::MockRenderer backend;
  ayt::ui::UIManager ui;
  AudioEditorSession session;
  std::filesystem::path assets =
      ayt::test::testTmpDir() / "audio-authoring-assets";
  WorkbenchFixture() {
    std::filesystem::create_directories(assets);
    if (!audio.initialize())
      throw std::runtime_error("audio initialize");
    ui.initialize(&backend);
    ui.setClientSize(1400, 1800);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();
    ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto root = std::filesystem::path(__FILE__)
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    if (!ui.loadLayout(
            (root / "EngineAssets/AYAudio/ui/audio_editor.ui.json").string()))
      throw std::runtime_error("layout");
    session.setAudio(&audio);
    session.setDriveAudioUpdate(false);
    session.setRecordingRootProvider([this] { return assets.string(); });
    if (!session.attach(ui))
      throw std::runtime_error("attach");
  }
  ~WorkbenchFixture() {
    session.detach();
    ui.shutdown();
    audio.shutdown();
  }
  template <class T> T *find(const char *id) {
    auto *item = dynamic_cast<T *>(ui.findById(id));
    if (!item)
      throw std::runtime_error(std::string("missing widget ") + id);
    return item;
  }
  void click(const char *id) {
    auto *item = find<ayt::ui::Button>(id);
    const auto b = item->getWorldBounds();
    ayt::ui::UIMouseEvent event({(b.minX + b.maxX) / 2, (b.minY + b.maxY) / 2},
                                0);
    item->onMouseMove(event);
    item->onMouseButtonDown(event);
    item->onMouseButtonUp(event);
  }
  void parameters() {
    auto *check = find<ayt::ui::CheckBox>("performance_parameters_toggle");
    check->setChecked(true); // programmatic set does not notify
    const auto b = check->getWorldBounds();
    ayt::ui::UIMouseEvent event({b.minX + 5, b.minY + 5}, 0);
    check->onMouseButtonDown(event);
    check->onMouseButtonUp(event);
    check->onMouseButtonDown(event);
    check->onMouseButtonUp(event);
    ui.layout();
  }
};
} // namespace
TEST_SUITE(AudioAuthoringTests)
TEST_CASE(
    MidiAndKeyboardSharePitchWithoutPrematureReleaseAndFocusLossClearsAll) {
  WorkbenchFixture f;
  auto *engine = f.audio.engine();
  CHECK(f.session.handleKey('A', true));
  CHECK(engine->activeVoiceCount() == 1);
  MidiEvent e;
  e.kind = MidiEvent::Kind::NoteOn;
  e.key = 60;
  e.value = 100;
  e.time = f.session.performanceTime();
  f.session.handleMidiEvent(e);
  CHECK(engine->activeVoiceCount() == 1);
  CHECK(f.session.handleKey('A', false));
  VoiceRuntime voice;
  CHECK(engine->voiceStateAt(0, voice));
  CHECK(voice.releaseFrames == 0);
  e.kind = MidiEvent::Kind::NoteOff;
  e.value = 0;
  f.session.handleMidiEvent(e);
  CHECK(engine->voiceStateAt(0, voice));
  CHECK(voice.releaseFrames > 0);
  e.kind = MidiEvent::Kind::NoteOn;
  e.key = 64;
  e.value = 100;
  e.channel = 3;
  f.session.handleMidiEvent(e);
  f.session.onFocusLost();
  CHECK(engine->activeVoiceCount() == 0);
}
TEST_CASE(MixerPresetRoundTripAndExternalChangeProtection) {
  WorkbenchFixture f;
  f.find<ayt::ui::TabControl>("audio_tabs")->setSelectedIndex(2);
  f.ui.layout();
  auto *engine = f.audio.engine();
  engine->setMasterGain(.4f);
  engine->setBusGain(AudioBus::Preview, .3f);
  AudioEffect limiter;
  limiter.kind = AudioEffectKind::Limiter;
  limiter.thresholdDb = -9;
  CHECK(engine->setBusEffects(AudioBus::Preview, {limiter}));
  f.click("mixer_preset_save");
  const auto file = f.assets / "audio/presets/mixer.json";
  CHECK(std::filesystem::is_regular_file(file));
  engine->setMasterGain(.9f);
  engine->setBusEffects(AudioBus::Preview, {});
  f.click("mixer_preset_load");
  CHECK_FLOAT_EQ(engine->masterGain(), .4f, 1e-6f);
  CHECK(engine->busEffects(AudioBus::Preview).size() == 1);
  const std::string external = "external edit";
  std::ofstream(file, std::ios::trunc) << external;
  f.click("mixer_preset_save");
  std::ifstream input(file);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  CHECK(bytes == external);
  f.find<ayt::ui::TextInput>("mixer_preset_path")->setText(L"../escape.json");
  f.click("mixer_preset_save");
  CHECK(!std::filesystem::exists(f.assets.parent_path() / "escape.json"));
}
TEST_CASE(
    InstrumentPresetAndCompensationPersistAndInvalidMixDoesNotMutateEngine) {
  WorkbenchFixture f;
  f.parameters();
  auto *offset = f.find<ayt::ui::TextInput>("audio_input_offset_ms");
  f.ui.setFocus(offset);
  offset->setText(L"35");
  offset->onKeyDown(13);
  CHECK_FLOAT_EQ(f.session.inputOffsetSeconds(), .035, 1e-6);
  f.click("instrument_preset_save");
  const auto file = f.assets / "audio/presets/instrument.json";
  CHECK(std::filesystem::is_regular_file(file));
  offset->setText(L"0");
  offset->onKeyDown(13);
  f.click("instrument_preset_load");
  CHECK_FLOAT_EQ(f.session.inputOffsetSeconds(), .035, 1e-6);
  auto *e = f.audio.engine();
  auto preset = authoring::MixerPreset::capture(*e);
  const auto old = e->masterGain();
  preset.master = .1f;
  preset.gains[3] = -1;
  bool rejected = false;
  try {
    preset.apply(*e);
  } catch (...) {
    rejected = true;
  }
  CHECK(rejected);
  CHECK(e->masterGain() == old);
}
TEST_SUITE_END
TEST_SUITE(AudioWorkbenchProcessingTests)
TEST_CASE(EffectControlsEditOnlySelectedBusAndSofaSelectionIsReversible) {
  WorkbenchFixture f;
  f.find<ayt::ui::TabControl>("audio_tabs")->setSelectedIndex(2);
  f.ui.layout();
  auto *engine=f.audio.engine();
  f.find<ayt::ui::ComboBox>("audio_effect_bus")->setSelectedIndex(int(AudioBus::Preview));
  f.find<ayt::ui::ComboBox>("audio_effect_kind")->setSelectedIndex(int(AudioEffectKind::Limiter));
  f.find<ayt::ui::TextInput>("fx_threshold")->setText(L"-8");
  f.click("audio_effect_add");
  CHECK(engine->busEffects(AudioBus::Preview).size()==1);
  CHECK(engine->busEffects(AudioBus::Preview)[0].thresholdDb==-8);
  CHECK(engine->busEffects(AudioBus::Master).empty());
  f.find<ayt::ui::ComboBox>("audio_effect_kind")->setSelectedIndex(int(AudioEffectKind::Delay));
  f.click("audio_effect_add");
  CHECK(engine->busEffects(AudioBus::Preview).size()==2);
  f.find<ayt::ui::ComboBox>("audio_effect_slot")->setSelectedIndex(1);
  f.click("audio_effect_up");
  CHECK(engine->busEffects(AudioBus::Preview)[0].kind==AudioEffectKind::Delay);
  f.find<ayt::ui::TextInput>("fx_wet")->setText(L"2");f.click("audio_effect_apply");
  CHECK(engine->busEffects(AudioBus::Preview)[0].wet<=1);
  const char* sofa=std::getenv("AY_AUDIO_SOFA_TEST_FILE");
  if(sofa){f.session.setPathPicker([sofa]{return std::string(sofa);});f.click("audio_sofa_load");CHECK(engine->debugSnapshot().listener.hrtfEnabled);f.click("audio_sofa_clear");CHECK(!engine->debugSnapshot().listener.hrtfEnabled);}
}
TEST_SUITE_END
