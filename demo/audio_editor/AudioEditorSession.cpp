#include "AudioEditorSession.h"

#include "AYUI/Button.h"
#include "AYUI/Slider.h"
#include "AYUI/TextLabel.h"
#include <AYUI/Style.h>

#include <AYAudio/AudioTypes.h>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <AYResource/assetsImpl/Audio.h>
#include <AYResource/Loader/AudioLoader.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <AYIO/File.h>
#include <AYUI/Box.h>
#include <AYUI/Panel.h>
#include <AYUI/ScrollView.h>
#include <AYUI/LeafWidget.h>
#include <AYUI/TabControl.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TextInput.h>
#include <AYUI/Theme.h>
#include <AYUI/Authoring/NumericFields.h>

namespace ayt::audio {
namespace {

void setLabelValue(ayt::ui::TextLabel* label, float v, bool asInt) {
    if (label == nullptr) {
        return;
    }
    wchar_t buf[32];
    if (asInt) {
        std::swprintf(buf, 32, L"%d", static_cast<int>(v + 0.5f));
    } else {
        std::swprintf(buf, 32, L"%.2f", v);
    }
    label->setText(buf);
}

} // namespace

#include "AudioWorkbench.inl"

AudioEditorSession::AudioEditorSession() = default;

AudioEditorSession::~AudioEditorSession() {
    detach();
}

bool AudioEditorSession::attach(ayt::ui::UIManager& ui) {
    detach();
    _ui = &ui;
    auto* mount = _ui->findById("audio_editor_root");
    if (!mount) return false;
    _workbench = std::make_unique<Workbench>(*this, mount);
    if (_audio == nullptr || _audio->engine() == nullptr
        || !_audio->engine()->isInitialized()) {
        setStatus(L"No AudioSubSystem / engine — register audio or drop -no-audio");
        // Workbench stays interactive while the engine is unavailable.
        return true;
    }

    if (!ensureClips()) {
        setStatus(L"Clip register failed");
        // No global engine mutation on attach.
        return true;
    }

    refreshStatus();
    return true;
}

void AudioEditorSession::detach() {
    releaseResources();
    unbindChrome(); _workbench.reset(); _ui=nullptr; _status=nullptr; _streamTrackLabel=nullptr;
}

void AudioEditorSession::tick(float dt) {
    if (_audio != nullptr
        && (_audio->engine() == nullptr || !_audio->engine()->isInitialized())) {
        // Play session teardown may have shut subsystems down.
        if ((_tickCount % 60) == 0) {
            setStatus(L"Audio engine stopped — reopen Audio Editor to re-init");
        }
        if (_workbench) _workbench->tick(dt);
        ++_tickCount;
        return;
    }
    if (liveEngine() && !ensureClips()) { setStatus(L"Audio resources could not be registered"); return; }
    if (_driveAudioUpdate && _audio != nullptr) {
        _audio->update(dt);
    }
    if (_workbench) _workbench->tick(dt);
    ++_tickCount;
    if ((_tickCount % 15) == 0) {
        refreshStatus();
        refreshTracks();
    }
}

AudioEngine* AudioEditorSession::liveEngine() const {
    auto* engine=_audio ? _audio->engine() : nullptr;
    return engine && engine->isInitialized() ? engine : nullptr;
}
void AudioEditorSession::releaseResources() {
    if (_workbench) _workbench->releaseAudio();
    auto* engine=liveEngine();
    const bool same=engine && engine==_boundEngine && engine->lifecycleGeneration()==_engineGeneration;
    if (!same && _instrument) _instrument->abandon();
    if (same) {
        stopOwned();
        for (auto id : {_clipA,_clipB,_clipLoop}) if (id) engine->releaseClip(id);
        for (auto id : _instrumentClips) engine->releaseClip(id);
    }
    _instrument.reset(); _instrumentClips.clear(); _ownedVoices.clear();
    _keyboard.fill(false); _mouse.fill(false); _mouseKey=-1;
    _stream={}; _streamPath.clear(); _loopVoice=InvalidVoice;
    _clipA=_clipB=_clipLoop=InvalidClipId; _clipsReady=false; _boundEngine=nullptr;
    _paused=false; _spaceDown=false; _pedalLatch=false;
}
bool AudioEditorSession::ensureClips() {
    auto* engine=liveEngine(); if (!engine) return false;
    if (_clipsReady && engine==_boundEngine && engine->lifecycleGeneration()==_engineGeneration) return true;
    releaseResources(); _boundEngine=engine; _engineGeneration=engine->lifecycleGeneration();
    _clipA=registerOwnedClip(_pcmA,440.0f,0.35f,0.35f);
    _clipB=registerOwnedClip(_pcmB,880.0f,0.25f,0.30f);
    _clipLoop=registerOwnedClip(_pcmLoop,220.0f,1.0f,0.20f);
    _clipsReady=_clipA && _clipB && _clipLoop;
    if (!_clipsReady) { releaseResources(); return false; }
    buildInstrument(_sine); return _instrument && !_instrumentClips.empty();
}

std::vector<float> AudioEditorSession::makeSine(uint32_t sampleRate,
                                                uint16_t channels,
                                                float freqHz, float seconds,
                                                float amp) {
    const uint32_t frames =
        static_cast<uint32_t>(static_cast<double>(sampleRate) * seconds);
    std::vector<float> pcm(static_cast<size_t>(frames) * channels, 0.0f);
    const double twoPiF =
        2.0 * 3.14159265358979323846 * static_cast<double>(freqHz);
    for (uint32_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(
            std::sin(twoPiF * static_cast<double>(i)
                     / static_cast<double>(sampleRate))
            * amp);
        for (uint16_t c = 0; c < channels; ++c) {
            pcm[static_cast<size_t>(i) * channels + c] = s;
        }
    }
    return pcm;
}

AudioClipId AudioEditorSession::registerOwnedClip(std::vector<float>& storage,
                                                  float freqHz, float seconds,
                                                  float amp) {
    AudioEngine* eng = liveEngine();
    if (eng == nullptr) {
        return InvalidClipId;
    }
    storage = makeSine(eng->outputSampleRate(), 1, freqHz, seconds, amp);
    AudioClipDesc desc{};
    desc.sampleRate = eng->outputSampleRate();
    desc.channels = 1;
    desc.bitsPerSample = 32;
    desc.format = AudioSampleFormat::F32;
    desc.frameCount = static_cast<uint32_t>(storage.size());
    desc.pcmData = storage.data();
    return eng->registerClip(desc);
}

ayt::ui::Button* AudioEditorSession::bindButton(const char* id,
                                                std::function<void()> fn) {
    if (_ui == nullptr) {
        return nullptr;
    }
    auto* b = dynamic_cast<ayt::ui::Button*>(_ui->findById(id));
    if (b != nullptr) {
        b->setOnClicked(std::move(fn));
    }
    return b;
}

ayt::ui::Slider* AudioEditorSession::bindSlider(
    const char* id, std::function<void(float)> fn) {
    if (_ui == nullptr) {
        return nullptr;
    }
    auto* s = dynamic_cast<ayt::ui::Slider*>(_ui->findById(id));
    if (s != nullptr) {
        s->setOnValueChanged(std::move(fn));
    }
    return s;
}

void AudioEditorSession::bindChrome() {
    if (_ui == nullptr) {
        return;
    }
    _status = dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("lbl_status"));
    _streamTrackLabel =
        dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("lbl_stream_track"));

    bindButton("btn_play_440", [this]() { if (ensureClips()) playBeep(_clipA, AudioBus::Sfx); });
    bindButton("btn_play_880", [this]() { if (ensureClips()) playBeep(_clipB, AudioBus::Ui); });
    bindButton("btn_loop_220", [this]() {
        AudioEngine* eng = liveEngine();
        if (!ensureClips() || eng == nullptr || _clipLoop == InvalidClipId) {
            return;
        }
        if (_loopVoice != InvalidVoice) {
            eng->stop(_loopVoice);
            _loopVoice = InvalidVoice;
        }
        _loopVoice = eng->play(_clipLoop, AudioBus::Music, {}, _previewGain, true);
        eng->setVoicePaused(_loopVoice,_paused);
        refreshStatus();
    });
    bindButton("btn_stop_all", [this]() {
        stopOwned();
        refreshStatus();
    });
    bindButton("btn_pause", [this]() { togglePause(); });
    bindButton("btn_open_stream", [this]() { openStream(); });
    bindButton("btn_stop_stream", [this]() { stopStream(); });
    bindButton("btn_spatial_l", [this]() { playSpatial(-4.0f); });
    bindButton("btn_spatial_c", [this]() { playSpatial(0.0f); });
    bindButton("btn_spatial_r", [this]() { playSpatial(4.0f); });
    bindButton("btn_duck", [this]() { toggleDuck(); });
    bindButton("btn_reverb_preset", [this]() { cycleReverbPreset(); });

    bindButton("btn_octave_down",[this](){ releaseKeys(); _octave=std::max(1,_octave-1); updateKeys(); });
    bindButton("btn_octave_up",[this](){ releaseKeys(); _octave=std::min(6,_octave+1); updateKeys(); });
    bindButton("btn_instrument",[this](){ if (ensureClips()) buildInstrument(!_sine); updateKeys(); });
    bindButton("btn_load_sample",[this](){ loadSample(); });
    bindButton("btn_release_notes",[this](){ releaseKeys(); });
    bindButton("btn_sustain",[this](){ if(!ensureClips()) return; _pedalLatch=!_pedalLatch; if (_instrument) _instrument->setSustain(_spaceDown || _pedalLatch); updateKeys(); });
    bindSlider("sld_velocity",[this](float v){_velocity=v/100.0f;});
    bindSlider("sld_preview",[this](float v){
        _previewGain=v/100.0f; if (!ensureClips()) return; if (_instrument) _instrument->setGain(_previewGain);
        if (auto* e=liveEngine()) { for (auto h:_ownedVoices) e->setVoiceVolume(h,_previewGain); e->setVoiceVolume(_loopVoice,_previewGain); e->setVoiceVolume(_stream.voice,_previewGain); }
    });
    updateKeys();

    auto* valMaster =
        dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("val_master"));
    auto* valMusic =
        dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("val_music"));
    auto* valSfx = dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("val_sfx"));
    auto* valReverb =
        dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("val_reverb"));
    auto* valTs =
        dynamic_cast<ayt::ui::TextLabel*>(_ui->findById("val_timescale"));

    bindSlider("sld_master", [this, valMaster](float v) {
        if (AudioEngine* eng = !_refreshing ? liveEngine() : nullptr) {
            eng->setMasterGain(v / 100.0f);
        }
        setLabelValue(valMaster, v, true);
    });
    bindSlider("sld_music", [this, valMusic](float v) {
        if (AudioEngine* eng = !_refreshing ? liveEngine() : nullptr) {
            eng->setBusGain(AudioBus::Music, v / 100.0f);
        }
        setLabelValue(valMusic, v, true);
    });
    bindSlider("sld_sfx", [this, valSfx](float v) {
        if (AudioEngine* eng = !_refreshing ? liveEngine() : nullptr) {
            eng->setBusGain(AudioBus::Sfx, v / 100.0f);
        }
        setLabelValue(valSfx, v, true);
    });
    bindSlider("sld_reverb", [this, valReverb](float v) {
        if (AudioEngine* eng = !_refreshing ? liveEngine() : nullptr) {
            eng->setMasterReverbReturn(v / 100.0f);
        }
        setLabelValue(valReverb, v, true);
    });
    bindSlider("sld_timescale", [this, valTs](float v) {
        const float scale = v / 100.0f;
        if (AudioEngine* eng = !_refreshing ? liveEngine() : nullptr) {
            eng->setTimeScale(scale);
        }
        setLabelValue(valTs, scale, false);
    });
}

void AudioEditorSession::unbindChrome() {
    if (_ui == nullptr) {
        return;
    }
    const char* buttonIds[] = {
        "btn_play_440",   "btn_play_880",      "btn_loop_220",
        "btn_stop_all",   "btn_pause",         "btn_open_stream",
        "btn_octave_down", "btn_octave_up", "btn_instrument", "btn_load_sample", "btn_release_notes", "btn_sustain",
        "btn_stop_stream","btn_spatial_l",     "btn_spatial_c",
        "btn_spatial_r",  "btn_duck",          "btn_reverb_preset",
    };
    for (const char* id : buttonIds) {
        if (auto* b = dynamic_cast<ayt::ui::Button*>(_ui->findById(id))) {
            b->setOnClicked({});
        }
    }
    const char* sliderIds[] = {
        "sld_velocity", "sld_preview", "sld_master", "sld_music", "sld_sfx", "sld_reverb", "sld_timescale",
    };
    for (const char* id : sliderIds) {
        if (auto* s = dynamic_cast<ayt::ui::Slider*>(_ui->findById(id))) {
            s->setOnValueChanged({});
        }
    }
}

void AudioEditorSession::setStatus(const std::wstring& text) {
    if (_status != nullptr) {
        _status->setText(text);
    }
}

void AudioEditorSession::setTrackRow(int index, const std::wstring& name,
                                     const std::wstring& info) {
    if (_ui == nullptr || index < 0 || index > 3) {
        return;
    }
    char nameId[32];
    char infoId[32];
    std::snprintf(nameId, sizeof(nameId), "track_name_%d", index);
    std::snprintf(infoId, sizeof(infoId), "track_info_%d", index);
    if (auto* n = dynamic_cast<ayt::ui::TextLabel*>(_ui->findById(nameId))) {
        n->setText(name);
    }
    if (auto* i = dynamic_cast<ayt::ui::TextLabel*>(_ui->findById(infoId))) {
        i->setText(info);
    }
}

void AudioEditorSession::refreshTracks() {
    AudioEngine* eng = liveEngine();
    int row = 0;
    if (_stream.voice != InvalidVoice) {
        std::wstring pathW;
        if (!_streamPath.empty()) {
            pathW.assign(_streamPath.begin(), _streamPath.end());
            if (pathW.size() > 48) {
                pathW = L"…" + pathW.substr(pathW.size() - 47);
            }
        } else {
            pathW = L"(stream)";
        }
        wchar_t info[96];
        std::swprintf(info, 96, L"Music  voice=%llu  stream=%u",
                      static_cast<unsigned long long>(_stream.voice), _stream.stream);
        setTrackRow(row++, L"BGM Stream", info);
        if (_streamTrackLabel != nullptr) {
            _streamTrackLabel->setText(L"Track: " + pathW);
        }
    } else if (_streamTrackLabel != nullptr) {
        _streamTrackLabel->setText(L"Track: (none)");
    }

    if (eng != nullptr) {
        for (uint32_t i = 0; row < 4; ++i) {
            VoiceRuntime snapshot;
            const VoiceRuntime* vr=&snapshot;
            if (!eng->voiceStateAt(i,snapshot)) {
                break;
            }
            const bool owned=vr->voiceId==_loopVoice || std::find(_ownedVoices.begin(),_ownedVoices.end(),vr->voiceId)!=_ownedVoices.end();
            if (!vr->playing || !owned) {
                continue;
            }
            if (_stream.voice != InvalidVoice
                && vr->sourceKind == VoiceSourceKind::Stream
                && vr->stream == _stream.stream) {
                continue;
            }
            const wchar_t* busName = L"Bus";
            switch (vr->bus) {
            case AudioBus::Master: busName = L"Master"; break;
            case AudioBus::Music: busName = L"Music"; break;
            case AudioBus::Sfx: busName = L"SFX"; break;
            case AudioBus::Ui: busName = L"UI"; break;
            case AudioBus::Voice: busName = L"Voice"; break;
            default: break;
            }
            wchar_t name[64];
            wchar_t info[96];
            if (vr->sourceKind == VoiceSourceKind::Stream) {
                std::swprintf(name, 64, L"Stream #%u", vr->stream);
            } else {
                std::swprintf(name, 64, L"Clip #%u", vr->clip);
            }
            std::swprintf(info, 96, L"%s  vol=%.2f  loop=%s", busName, vr->volume,
                          vr->loop ? L"yes" : L"no");
            setTrackRow(row++, name, info);
        }
    }

    while (row < 4) {
        setTrackRow(row++, L"—", L"");
    }
}

void AudioEditorSession::refreshStatus() {
    auto* eng=liveEngine();
    if (!eng) {setStatus(L"音频引擎不可用");return;}
    if(eng->isPaused())setStatus(L"引擎输出已暂停 · 混音器中恢复");
    else if(eng->masterGain()==0)setStatus(L"总输出为 0% · 已静音");
    else if(_workbench && _workbench->tabs->getSelectedIndex()==0 && eng->busGain(Workbench::bus(_workbench->instrumentBus->getSelectedIndex()))==0)setStatus(L"演奏分组为 0% · 已静音");
    else if(_workbench && _workbench->tabs->getSelectedIndex()==0 && _previewGain==0)setStatus(L"演奏音量为 0% · 已静音");
    else setStatus(eng->activeVoiceCount()?L"输出就绪 · 正在发声":L"输出就绪 · 等待演奏或试听");
}

void AudioEditorSession::playBeep(AudioClipId clip, AudioBus bus) {
    AudioEngine* eng = liveEngine();
    if (!ensureClips() || eng == nullptr || clip == InvalidClipId) {
        setStatus(L"No engine / clip");
        return;
    }
    const auto v=eng->play(clip,bus,{},_previewGain,false);
    if (v) { _ownedVoices.push_back(v); eng->setVoiceEnvelope(v,0.004f,0.015f); eng->setVoicePaused(v,_paused); }
    refreshStatus();
}

void AudioEditorSession::playSpatial(float x) {
    AudioEngine* eng = liveEngine();
    if (!ensureClips() || eng == nullptr || _clipA == InvalidClipId) {
        return;
    }
    SpatializationParams sp{};
    sp.mode = SpatializationParams::Mode::World;
    sp.position = {x, 0, -2};
    sp.referenceDistance = 1.0f;
    sp.maxDistance = 40.0f;
    sp.spatialBlend = 1.0f;
    sp.reverbSend = 0.35f;
    const auto v=eng->play(_clipA,AudioBus::Sfx,sp,_previewGain,false);
    if (v) { _ownedVoices.push_back(v); eng->setVoiceEnvelope(v,0.004f,0.015f); eng->setVoicePaused(v,_paused); }
    refreshStatus();
}

void AudioEditorSession::openStream() {
    if (_audio == nullptr || _audio->engine() == nullptr) {
        setStatus(L"No audio engine");
        return;
    }
    if (!_pathPicker) {
        setStatus(L"No path picker (host must setPathPicker)");
        return;
    }
    if (!ensureClips()) return;
    releaseKeys();
    const std::string path = _pathPicker();
    if (path.empty() || !ensureClips()) {
        setStatus(L"Stream cancelled");
        return;
    }
    if (_stream.voice != InvalidVoice) {
        _audio->stopStreaming(_stream);
        _stream = {};
    }
    _stream = _audio->playStreamingFile(path, AudioBus::Music, true, _previewGain);
    if (_stream.voice == InvalidVoice) {
        _streamPath.clear();
        setStatus(L"Stream open failed — check .wav/.mp3/.ogg + path");
        refreshTracks();
        return;
    }
    _streamPath = path;
    AudioEngine* eng = _audio->engine();
    eng->setVoicePaused(_stream.voice,_paused);
    setStatus(L"Streaming");
    refreshTracks();
}

void AudioEditorSession::stopStream() {
    if (liveEngine() == _boundEngine && liveEngine() && liveEngine()->lifecycleGeneration()==_engineGeneration && _stream.voice != InvalidVoice) {
        _audio->stopStreaming(_stream);
    }
    _stream = {};
    _streamPath.clear();
    refreshStatus();
    refreshTracks();
}

void AudioEditorSession::togglePause() {
    if (!ensureClips()) return;
    AudioEngine* eng = liveEngine();
    if (eng == nullptr) {
        return;
    }
    _paused=!_paused;
    for (auto v:_ownedVoices) eng->setVoicePaused(v,_paused);
    eng->setVoicePaused(_loopVoice,_paused); eng->setVoicePaused(_stream.voice,_paused);
    if (_instrument) _instrument->setPaused(_paused);
    if (_ui) if (auto* b=dynamic_cast<ayt::ui::Button*>(_ui->findById("btn_pause"))) b->setText(_paused ? L"Resume Preview" : L"Pause Preview");
    refreshStatus();
}

void AudioEditorSession::toggleDuck() {
    AudioEngine* eng = liveEngine();
    if (eng == nullptr) {
        return;
    }
    _ducked = !(eng->duckingState().depthDb<0);
    if (_ducked) {
        eng->setDucking(-12.0f, 40.0f, 8.0f);
    } else {
        eng->setDucking(0.0f, 40.0f, 8.0f);
    }
    refreshStatus();
}

void AudioEditorSession::cycleReverbPreset() {
    AudioEngine* eng = liveEngine();
    if (eng == nullptr) {
        return;
    }
    _reverbPreset = static_cast<uint8_t>((eng->reverbPreset() + 1) % 2);
    eng->setReverbPreset(_reverbPreset);
    refreshStatus();
}

void AudioEditorSession::stopOwned() {
    releaseKeys();
    auto* e=liveEngine();
    if (e && e==_boundEngine && e->lifecycleGeneration()==_engineGeneration) {
        for (auto h:_ownedVoices) e->stop(h);
        e->stop(_loopVoice);
    }
    _ownedVoices.clear(); _loopVoice=InvalidVoice; stopStream();
}
void AudioEditorSession::buildInstrument(bool sine) {
    auto* e=liveEngine(); if (!e) return;
    releaseKeys(); _instrument.reset();
    for (auto c:_instrumentClips) e->releaseClip(c); _instrumentClips.clear();
    _instrument=std::make_unique<SampleInstrument>(*e,16); _instrument->setGain(_previewGain); _instrument->setPaused(_paused);
    _sine=sine; _instrumentName=sine ? L"Sine" : L"Piano";
    bool fallback=false;
    for (int octave=1; octave<=7; ++octave) {
        const int note=(octave+1)*12;
        AudioClipId clip=InvalidClipId;
        if (!sine && !_assetRoot.empty()) {
            ayt::resource::AudioLoader loader;
            const auto base=std::filesystem::path(_assetRoot)/"samples"/("C"+std::to_string(octave));
            auto resource=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(base.string()+".aud"));
            if (!resource) resource=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(base.string()+".mp3"));
            if (resource) clip=_audio->registerClipFromResource(resource.get());
        }
        if (!clip) {
            fallback=true;
            const auto rate=e->outputSampleRate(); const uint32_t frames=rate*3;
            std::vector<float> pcm(frames);
            const double hz=440.0*std::exp2((note-69)/12.0);
            for (uint32_t f=0; f<frames; ++f) {
                const double t=static_cast<double>(f)/rate;
                double sample=std::sin(6.283185307179586*hz*t);
                if (!sine) sample=sample*std::exp(-t*1.4)+0.35*std::sin(6.283185307179586*hz*2*t)*std::exp(-t*3)+0.12*std::sin(6.283185307179586*hz*3*t)*std::exp(-t*5);
                pcm[f]=static_cast<float>(sample*0.22*std::min(1.0,t/0.004)*std::min(1.0,(3.0-t)/0.02));
            }
            AudioClipDesc desc{}; desc.sampleRate=rate; desc.channels=1; desc.frameCount=frames; desc.pcmData=pcm.data();
            clip=e->registerClip(desc);
        }
        if (clip) { _instrumentClips.push_back(clip); _instrument->addRegion({clip,note,std::max(0,note-6),std::min(127,note+6)}); }
    }
    if (!sine && fallback) _instrumentName=L"Piano (generated fallback)";
    if (_workbench) {_workbench->piano->low=18;_workbench->piano->high=102;_workbench->applyInstrument();}
}
void AudioEditorSession::loadSample() {
    if (!_pathPicker || !ensureClips()) return;
    releaseKeys(); const auto path=_pathPicker(); if (path.empty() || !ensureClips()) return;
    ayt::resource::AudioLoader loader;
    auto resource=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(path));
    if (!resource) {setStatus(L"Sample could not be decoded; choose WAV, MP3, OGG or cooked audio");return;}
    auto* e=liveEngine(); const auto clip=_audio->registerClipFromResource(resource.get());
    if (!clip) {setStatus(L"Sample registration failed (clip capacity or PCM format)");return;}
    _instrument.reset(); for (auto c:_instrumentClips) e->releaseClip(c); _instrumentClips={clip};
    _instrument=std::make_unique<SampleInstrument>(*e,16); _instrument->setGain(_previewGain); _instrument->setPaused(_paused);
    const int root=(_octave+1)*12; _instrument->addRegion({clip,root,std::max(0,root-12),std::min(127,root+24)});
    _instrumentName=L"Sample (root C"+std::to_wstring(_octave)+L")";
    if (_workbench) {_workbench->piano->low=std::max(0,root-12);_workbench->piano->high=std::min(127,root+24);_workbench->applyInstrument();}
    updateKeys();
}
void AudioEditorSession::pressPitch(int pitch,bool keyboard,bool down) {
    if (pitch<0 || pitch>127) return;
    if (down) { if (!ensureClips() || !_instrument) return; }
    else if (!_instrument || liveEngine()!=_boundEngine || !liveEngine() || liveEngine()->lifecycleGeneration()!=_engineGeneration) {
        (keyboard ? _keyboard[pitch] : _mouse[pitch])=false; return;
    }
    const bool before=_keyboard[pitch] || _mouse[pitch];
    (keyboard ? _keyboard[pitch] : _mouse[pitch])=down;
    const bool after=_keyboard[pitch] || _mouse[pitch];
    if (!before && after && !_instrument->noteOn(pitch,_velocity)) {
        (keyboard ? _keyboard[pitch] : _mouse[pitch])=false;
        setStatus(L"音符不可用：超出音源范围或声部容量");
    }
    if (before && !after) _instrument->noteOff(pitch);
    updateKeys();
    if (_workbench) _workbench->noteStatus->setText(L"实际音高："+workbench::noteName(pitch));
}
void AudioEditorSession::pressNote(int index,bool keyboard,bool down) {
    if (index>=0 && index<13) pressPitch((_octave+1)*12+index+(_workbench?_workbench->shift:0),keyboard,down);
}
bool AudioEditorSession::handleKey(int key,bool pressed) {
    constexpr int keys[13]={'A','W','S','E','D','F','T','G','Y','H','U','J','K'};
    if (!_workbench || _workbench->tabs->getSelectedIndex()!=0 || !_workbench->keyboardEnabled->isChecked()) return false;
    if (pressed && _ui && ((_ui->getFocusedWidget() && _ui->getFocusedWidget()->isTextEditingWidget()) || (_ui->getModifiers()&6u))) return false;
    if (key==32) { if (!ensureClips()) return false;_spaceDown=pressed;if (_instrument) _instrument->setSustain(pressed||_pedalLatch);updateKeys();return true; }
    for (int i=0;i<13;++i) if (key==keys[i]) {const int pitch=(_octave+1)*12+i+_workbench->shift;if(pitch<0||pitch>127)return false;if(!pressed&&!_keyboard[pitch])return false;pressPitch(pitch,true,pressed);return true;}
    return false;
}
bool AudioEditorSession::handleMouseButton(float x,float y,int button,bool pressed) {
    if (!_workbench || _workbench->tabs->getSelectedIndex()!=0) return false;
    if (pressed) for (auto* parent=_workbench->piano->getParent();parent;parent=parent->getParent()) {
        if (auto* scroll=dynamic_cast<ayt::ui::ScrollView*>(parent);scroll&&!scroll->getClientRect().contains({x,y}))return false;
    }
    const ayt::ui::UIMouseEvent e({x,y},button);
    return pressed?_workbench->piano->onMouseButtonDown(e):_workbench->piano->onMouseButtonUp(e);
}
bool AudioEditorSession::handleMouseMove(float x,float y) {
    return _workbench && _workbench->piano->onMouseMove({{x,y}});
}
void AudioEditorSession::onFocusLost() {
    releaseKeys();
    if (_ui) {_ui->cancelCapture();_ui->onKeyUp(16);_ui->onKeyUp(17);_ui->onKeyUp(18);}
}
bool AudioEditorSession::openResource(const std::string& path) {return _workbench&&_workbench->loadResource(path);}
ayt::math::FRectangle AudioEditorSession::pianoKeyBounds(int note) const {return _workbench?_workbench->piano->keyBounds(note):ayt::math::FRectangle{};}

void AudioEditorSession::releaseKeys() {
    if (_workbench) _workbench->piano->cancel();
    _keyboard.fill(false); _mouse.fill(false); _mouseKey=-1; _spaceDown=false; _pedalLatch=false;
    if (_instrument) {
        const auto* e=liveEngine();
        if (!e || e!=_boundEngine || e->lifecycleGeneration()!=_engineGeneration) _instrument->abandon();
        else _instrument->allNotesOff();
    } updateKeys();
}
void AudioEditorSession::updateKeys() {
    if (!_workbench) return;
    auto* p=_workbench->piano;p->octave=_octave;p->transpose=_workbench->shift;p->markDirty();
    _workbench->instrument->setText(_instrumentName+L" · 键盘 C"+std::to_wstring(_octave)+L" · 移调 "+std::to_wstring(_workbench->shift)+L" 半音");
    if (auto* b=dynamic_cast<ayt::ui::Button*>(_ui->findById("btn_sustain"))) b->setText((_pedalLatch||_spaceDown)?L"延音开启":L"延音关闭");
}

} // namespace ayt::audio
