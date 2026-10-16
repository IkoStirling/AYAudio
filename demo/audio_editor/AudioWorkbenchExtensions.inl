// Included in Workbench, sharing the same owner-thread lifetime as the
// transport.
Combo *fxBus = nullptr, *fxKind = nullptr, *fxSlot = nullptr;
ayt::ui::authoring::NumericFields *fxFields = nullptr;
ayt::ui::authoring::NumericFields *fxTimeFields = nullptr;
AudioBus selectedFxBus() const {
  return AudioBus(
      std::clamp(fxBus->getSelectedIndex(), 0, int(AudioBus::Count) - 1));
}
void refreshEffects() {
  if (!fxSlot)
    return;
  const auto *e = s.liveEngine();
  const auto chain =
      e ? e->busEffects(selectedFxBus()) : std::vector<AudioEffect>{};
  static const wchar_t *names[] = {L"低通", L"高通", L"参数均衡",
                                   L"压缩", L"延迟", L"限幅"};
  std::vector<std::wstring> items;
  for (size_t i = 0; i < chain.size(); ++i)
    items.push_back(std::to_wstring(i + 1) + L" · " +
                    names[int(chain[i].kind)]);
  if (items.empty())
    items.push_back(L"空效果链");
  fxSlot->setItems(items);
  fxSlot->setSelectedIndex(0);
  readEffect();
}
void readEffect() {
  if (!fxFields)
    return;
  auto *e = s.liveEngine();
  if (!e)
    return;
  const auto chain = e->busEffects(selectedFxBus());
  const int index = fxSlot->getSelectedIndex();
  if (index < 0 || index >= int(chain.size()))
    return;
  const auto &effect = chain[index];
  fxKind->setSelectedIndex(int(effect.kind));
  fxFields->setValues({effect.frequency, effect.q, effect.gainDb,
                       effect.thresholdDb, effect.ratio});
  fxTimeFields->setValues({effect.attack * 1000, effect.release * 1000,
                           effect.delay * 1000, effect.feedback, effect.wet});
}
void changeEffect(int action) {
  try {
    auto *e = s.liveEngine();
    if (!e)
      throw std::runtime_error("audio engine is unavailable");
    auto chain = e->busEffects(selectedFxBus());
    const int index = fxSlot->getSelectedIndex();
    if (action < 2) {
      std::vector<float> values, times;
      if (!fxFields->readValues(5, values) ||
          !fxTimeFields->readValues(5, times))
        throw std::runtime_error("invalid effect parameters");
      AudioEffect effect;
      effect.kind = AudioEffectKind(fxKind->getSelectedIndex());
      effect.frequency = values[0];
      effect.q = values[1];
      effect.gainDb = values[2];
      effect.thresholdDb = values[3];
      effect.ratio = values[4];
      effect.attack = times[0] * .001f;
      effect.release = times[1] * .001f;
      effect.delay = times[2] * .001f;
      effect.feedback = times[3];
      effect.wet = times[4];
      if (action == 0)
        chain.push_back(effect);
      else if (index >= 0 && index < int(chain.size()))
        chain[index] = effect;
      else
        return;
    } else if (index >= 0 && index < int(chain.size())) {
      if (action == 2)
        chain.erase(chain.begin() + index);
      if (action == 3 && index > 0)
        std::swap(chain[index], chain[index - 1]);
      if (action == 4 && index + 1 < int(chain.size()))
        std::swap(chain[index], chain[index + 1]);
    }
    std::string error;
    if (!e->setBusEffects(selectedFxBus(), chain, &error))
      throw std::runtime_error(error);
    refreshEffects();
    s.setStatus(L"效果链已更新");
  } catch (const std::exception &e) {
    s.setStatus(wide(e.what()));
  }
}
void buildEffects(VBox *parent) {
  using namespace workbench;
  parent->addWidget(label(L"插入效果 · 从上到下处理 · Master 位于混响之后"),
                    28);
  auto *r = row(parent);
  fxBus = new Combo();
  fxBus->setId("audio_effect_bus");
  fxBus->setItems({L"Master", L"Music", L"SFX", L"UI", L"Voice", L"Preview"});
  r->addWidget(fxBus, 120);
  fxSlot = new Combo();
  fxSlot->setId("audio_effect_slot");
  r->addWidget(fxSlot, 190);
  fxKind = new Combo();
  fxKind->setId("audio_effect_kind");
  fxKind->setItems({L"低通", L"高通", L"参数均衡", L"压缩", L"延迟", L"限幅"});
  fxKind->setSelectedIndex(0);
  r->addWidget(fxKind, 130);
  button(r, L"添加", "audio_effect_add", [this] { changeEffect(0); }, 65);
  button(r, L"应用", "audio_effect_apply", [this] { changeEffect(1); }, 65);
  button(r, L"移除", "audio_effect_remove", [this] { changeEffect(2); }, 65);
  auto *labels = row(parent, 24);
  for (const auto *title :
       {L"频率 Hz", L"Q", L"增益 dB", L"阈值 dB", L"压缩比"})
    labels->addWidget(label(title));
  auto *params = row(parent, 32);
  fxFields = new ayt::ui::authoring::NumericFields(
      {"fx_frequency", "fx_q", "fx_gain", "fx_threshold", "fx_ratio"});
  fxFields->setValues({1000, .7071f, 0, -12, 4});
  params->addWidget(fxFields);
  auto *times = row(parent, 24);
  for (const auto *title :
       {L"起音 ms", L"释放 ms", L"延迟 ms", L"反馈", L"湿声"})
    times->addWidget(label(title));
  auto *timing = row(parent, 32);
  fxTimeFields = new ayt::ui::authoring::NumericFields(
      {"fx_attack", "fx_release", "fx_delay", "fx_feedback", "fx_wet"});
  fxTimeFields->setValues({10, 100, 150, .25f, .3f});
  timing->addWidget(fxTimeFields);
  auto *order = row(parent);
  button(order, L"前移", "audio_effect_up", [this] { changeEffect(3); }, 70);
  button(order, L"后移", "audio_effect_down", [this] { changeEffect(4); }, 70);
  button(
      order, L"载入 SOFA 双耳配置", "audio_sofa_load",
      [this] {
        try {
          auto *e = s.liveEngine();
          if (!e || !s._pathPicker)
            return;
          const auto path = s._pathPicker();
          if (path.empty())
            return;
          auto profile = std::make_shared<HrtfProfile>();
          std::string error;
          if (!profile->loadSofa(path, e->outputSampleRate(), &error) ||
              !e->setHrtfProfile(profile))
            throw std::runtime_error(error);
          auto listener = e->debugSnapshot().listener;
          listener.hrtfEnabled = true;
          e->setListener(listener);
          s.setStatus(L"SOFA 双耳配置已载入 · 对空间声部生效");
        } catch (const std::exception &e) {
          s.setStatus(wide(e.what()));
        }
      },
      180);
  button(
      order, L"关闭双耳卷积", "audio_sofa_clear",
      [this] {
        if (auto *e = s.liveEngine()) {
          e->setHrtfProfile(nullptr);
          auto listener = e->debugSnapshot().listener;
          listener.hrtfEnabled = false;
          e->setListener(listener);
        }
      },
      130);
  fxBus->setOnSelectionChanged([this](int) { refreshEffects(); });
  fxSlot->setOnSelectionChanged([this](int) { readEffect(); });
  refreshEffects();
}
bool calibrating = false;
double calibrationStart = 0;
AudioEngine *calibrationEngine = nullptr;
uint64_t calibrationGeneration = 0;
AudioClipId calibrationClip = 0;
VoiceHandle calibrationVoice = 0;
std::vector<float> calibrationProbe;
uint64_t calibrationOrigin = 0;
void clearCalibration() {
  if (s.liveEngine() == calibrationEngine && calibrationEngine &&
      calibrationEngine->lifecycleGeneration() == calibrationGeneration) {
    calibrationEngine->stop(calibrationVoice);
    calibrationEngine->releaseClip(calibrationClip);
  }
  calibrating = false;
  calibrationClip = 0;
  calibrationVoice = 0;
  calibrationEngine = nullptr;
}
void beginCalibration() {
  try {
    if (recorder.status().recording || recorder.status().playing ||
        (recorder.status().frames && !recordSaved))
      throw std::runtime_error(
          "save or discard the current take before calibration");
    auto *e = s.liveEngine();
    if (!e)
      throw std::runtime_error("audio engine is unavailable");
    clearCalibration();
    recordMonitor->setChecked(false);
    startRecording();
    if (!recorder.status().recording)
      return;
    calibrationProbe.resize(256);
    uint32_t random = 0x9e3779b9;
    for (auto &v : calibrationProbe) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      v = float(int(random % 2001) - 1000) * .0002f;
    }
    AudioClipDesc desc;
    desc.sampleRate = 48000;
    desc.channels = 1;
    desc.frameCount = uint32_t(calibrationProbe.size());
    desc.pcmData = calibrationProbe.data();
    calibrationEngine = e;
    calibrationGeneration = e->lifecycleGeneration();
    calibrationClip = e->registerClip(desc);
    recorder.update();
    calibrationOrigin = recorder.status().frames;
    calibrationVoice = e->play(calibrationClip, AudioBus::Preview);
    if (!calibrationVoice) {
      recorder.stop();
      throw std::runtime_error("cannot play calibration probe");
    }
    calibrationStart = s.performanceTime();
    calibrating = true;
    recordingMessage =
        L"正在测量往返延迟 · 输出使用引擎当前设备；请选择对应的回环或输入";
  } catch (const std::exception &e) {
    clearCalibration();
    recordingMessage = wide(e.what());
  }
}
void tickCalibration() {
  if (!calibrating)
    return;
  if (s.liveEngine() != calibrationEngine || !s.liveEngine() ||
      s.liveEngine()->lifecycleGeneration() != calibrationGeneration) {
    recorder.stop();
    clearCalibration();
    recordingMessage = L"引擎已更换，校准已取消";
    return;
  }
  if (s.performanceTime() - calibrationStart < .8 &&
      recorder.status().recording)
    return;
  recorder.stop();
  const auto state = recorder.status();
  const auto &samples = recorder.samples();
  const auto origin = std::min(calibrationOrigin, state.frames);
  std::vector<float> mono(size_t(state.frames - origin));
  for (size_t f = 0; f < mono.size(); ++f) {
    for (size_t c = 0; c < state.channels; ++c)
      mono[f] += samples[(f + origin) * state.channels + c] / state.channels;
  }
  AudioLatencyEstimate result;
  std::string error;
  const auto maxLag = uint32_t(std::min<size_t>(
      state.sampleRate / 2, mono.size() > calibrationProbe.size()
                                ? mono.size() - calibrationProbe.size()
                                : 0));
  if (estimateAudioLatency(calibrationProbe, mono, state.sampleRate, 0, maxLag,
                           result, &error))
    recordingMessage =
        L"往返延迟 " + workbench::number(result.seconds * 1000, 2) +
        L" ms · 相关度 " + workbench::number(result.confidence * 100, 1) +
        L"% · 不自动作为单向输入补偿";
  else
    recordingMessage = L"校准未完成：" + wide(error);
  clearCalibration();
}
