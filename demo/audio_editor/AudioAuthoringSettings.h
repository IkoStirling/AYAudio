#pragma once
#include "PerformanceDocument.h"
#include <AYAudio/AudioEngine.h>
namespace ayt::audio::authoring {
inline nlohmann::json effectJson(const AudioEffect &e) {
  return {{"kind", int(e.kind)},
          {"frequency", e.frequency},
          {"q", e.q},
          {"gainDb", e.gainDb},
          {"thresholdDb", e.thresholdDb},
          {"ratio", e.ratio},
          {"attack", e.attack},
          {"release", e.release},
          {"delay", e.delay},
          {"feedback", e.feedback},
          {"wet", e.wet}};
}
inline AudioEffect effectFromJson(const nlohmann::json &j) {
  AudioEffect e;
  e.kind = AudioEffectKind(j.at("kind").get<int>());
  e.frequency = j.at("frequency");
  e.q = j.at("q");
  e.gainDb = j.at("gainDb");
  e.thresholdDb = j.at("thresholdDb");
  e.ratio = j.at("ratio");
  e.attack = j.at("attack");
  e.release = j.at("release");
  e.delay = j.at("delay");
  e.feedback = j.at("feedback");
  e.wet = j.at("wet");
  return e;
}
inline constexpr const char *BusNames[] = {"Master", "Music", "SFX",
                                           "UI",     "Voice", "Preview"};
struct MixerPreset {
  float master = 1, reverb = .25f;
  uint8_t reverbPreset = 0;
  DuckingState duck;
  std::array<float, size_t(AudioBus::Count)> gains{1, 1, 1, 1, 1, 1};
  std::array<std::vector<AudioEffect>, size_t(AudioBus::Count)> effects;
  static MixerPreset capture(const AudioEngine &engine) {
    MixerPreset p;
    p.master = engine.masterGain();
    p.reverb = engine.reverbReturn();
    p.reverbPreset = engine.reverbPreset();
    p.duck = engine.duckingState();
    for (size_t i = 0; i < p.gains.size(); ++i) {
      p.gains[i] = engine.busGain(AudioBus(i));
      p.effects[i] = engine.busEffects(AudioBus(i));
    }
    return p;
  }
  void validate(uint32_t rate) const {
    auto number = [](float v, float lo, float hi) {
      return std::isfinite(v) && v >= lo && v <= hi;
    };
    if (!number(master, 0, 8) || !number(reverb, 0, 1) || reverbPreset > 1 ||
        !number(duck.depthDb, -96, 0) ||
        !number(duck.attackPerSec, .001f, 10000) ||
        !number(duck.releasePerSec, .001f, 10000))
      throw std::runtime_error("invalid mixer preset");
    for (size_t i = 0; i < gains.size(); ++i) {
      AudioEffectChain chain;
      if (!number(gains[i], 0, 8) || !chain.configure(effects[i], rate))
        throw std::runtime_error("invalid bus gain or effects");
    }
  }
  void apply(AudioEngine &engine) const {
    if (!engine.isInitialized())
      throw std::runtime_error("audio engine is not initialized");
    validate(engine.outputSampleRate());
    for (size_t i = 0; i < gains.size(); ++i) {
      engine.setBusEffects(AudioBus(i), effects[i]);
      engine.setBusGain(AudioBus(i), gains[i]);
    }
    engine.setMasterGain(master);
    engine.setMasterReverbReturn(reverb);
    engine.setReverbPreset(reverbPreset);
    engine.setDucking(duck.depthDb, duck.attackPerSec, duck.releasePerSec);
  }
  nlohmann::json json() const {
    validate(192000);
    nlohmann::json buses = nlohmann::json::object();
    for (size_t i = 0; i < gains.size(); ++i) {
      nlohmann::json chain = nlohmann::json::array();
      for (const auto &e : effects[i])
        chain.push_back(effectJson(e));
      buses[BusNames[i]] = {{"gain", gains[i]}, {"effects", chain}};
    }
    return {{"schema", "ay.audio.mixer-preset"},
            {"version", 1},
            {"master", master},
            {"reverb", reverb},
            {"reverbPreset", reverbPreset},
            {"duck",
             {{"depthDb", duck.depthDb},
              {"attackPerSec", duck.attackPerSec},
              {"releasePerSec", duck.releasePerSec}}},
            {"buses", buses}};
  }
  static MixerPreset fromJson(const nlohmann::json &j) {
    if (j.at("schema") != "ay.audio.mixer-preset" || j.at("version") != 1)
      throw std::runtime_error("unsupported mixer preset");
    MixerPreset p;
    p.master = j.at("master");
    p.reverb = j.at("reverb");
    const int preset = j.at("reverbPreset");
    if (preset < 0 || preset > 1)
      throw std::runtime_error("invalid reverb preset");
    p.reverbPreset = uint8_t(preset);
    p.duck.depthDb = j.at("duck").at("depthDb");
    p.duck.attackPerSec = j.at("duck").at("attackPerSec");
    p.duck.releasePerSec = j.at("duck").at("releasePerSec");
    for (size_t i = 0; i < p.gains.size(); ++i) {
      const auto &bus = j.at("buses").at(BusNames[i]);
      p.gains[i] = bus.at("gain");
      const auto &list = bus.at("effects");
      if (!list.is_array() || list.size() > 8)
        throw std::runtime_error("invalid effect list");
      for (const auto &e : list)
        p.effects[i].push_back(effectFromJson(e));
    }
    p.validate(192000);
    return p;
  }
};
struct InstrumentPreset {
  performance::Instrument instrument;
  double inputOffset = 0;
  nlohmann::json json() const {
    validate();
    const auto &i = instrument;
    return {{"schema", "ay.audio.instrument-preset"},
            {"version", 1},
            {"kind", i.kind},
            {"source", i.source},
            {"relative", i.relative},
            {"root", i.root},
            {"gain", i.gain},
            {"attack", i.attack},
            {"release", i.release},
            {"inputOffsetSeconds", inputOffset}};
  }
  void validate() const {
    const auto &i = instrument;
    if ((i.kind != "piano" && i.kind != "sine" && i.kind != "sample") ||
        (i.kind == "sample" && i.source.empty()) || i.source.size() > 4096 ||
        i.root < 0 || i.root > 127 || !std::isfinite(i.gain) || i.gain < 0 ||
        i.gain > 2 || !std::isfinite(i.attack) || i.attack < 0 ||
        i.attack > 2 || !std::isfinite(i.release) || i.release < 0 ||
        i.release > 10 || !std::isfinite(inputOffset) ||
        std::abs(inputOffset) > .5)
      throw std::runtime_error("invalid instrument preset");
  }
  static InstrumentPreset fromJson(const nlohmann::json &j) {
    if (j.at("schema") != "ay.audio.instrument-preset" || j.at("version") != 1)
      throw std::runtime_error("unsupported instrument preset");
    InstrumentPreset p;
    auto &i = p.instrument;
    i.kind = j.at("kind");
    i.source = j.at("source");
    i.relative = j.at("relative");
    i.root = j.at("root");
    i.gain = j.at("gain");
    i.attack = j.at("attack");
    i.release = j.at("release");
    p.inputOffset = j.at("inputOffsetSeconds");
    p.validate();
    return p;
  }
};
} // namespace ayt::audio::authoring
