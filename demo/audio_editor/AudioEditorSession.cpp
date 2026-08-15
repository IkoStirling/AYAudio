#include "AudioEditorSession.h"

#include "AYUI/Button.h"
#include "AYUI/Slider.h"
#include "AYUI/TextLabel.h"

#include <AYAudio/AudioTypes.h>

#include <cmath>
#include <cstdio>
#include <cstring>

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

AudioEditorSession::~AudioEditorSession() {
    detach();
}

bool AudioEditorSession::attach(ayt::ui::UIManager& ui) {
    detach();
    _ui = &ui;
    if (_audio == nullptr || _audio->engine() == nullptr
        || !_audio->engine()->isInitialized()) {
        setStatus(L"No AudioSubSystem / engine — register audio or drop -no-audio");
        bindChrome(); // still bind so UI is not dead; actions no-op safely
        return true;
    }

    AudioEngine* eng = _audio->engine();
    eng->setMasterGain(1.0f);
    eng->setBusGain(AudioBus::Music, 1.0f);
    eng->setBusGain(AudioBus::Sfx, 1.0f);
    eng->setMasterReverbReturn(0.15f);
    eng->setReverbPreset(0);
    eng->setDopplerScale(1.0f);

    ListenerParams listener{};
    listener.position = {0, 0, 0};
    listener.forward = {0, 0, -1};
    listener.up = {0, 1, 0};
    eng->setListener(listener);

    if (!ensureClips()) {
        setStatus(L"Clip register failed");
        bindChrome();
        return true;
    }

    bindChrome();
    setStatus(L"Ready — Play 440 (device check) → Open Stream…");
    return true;
}

void AudioEditorSession::detach() {
    if (_audio != nullptr && _stream.voice != InvalidVoice) {
        _audio->stopStreaming(_stream);
        _stream = {};
    }
    if (_audio != nullptr && _audio->engine() != nullptr
        && _loopVoice != InvalidVoice) {
        _audio->engine()->stop(_loopVoice);
        _loopVoice = InvalidVoice;
    }
    unbindChrome();
    _ui = nullptr;
    _status = nullptr;
    _streamTrackLabel = nullptr;
}

void AudioEditorSession::tick(float dt) {
    if (_audio != nullptr
        && (_audio->engine() == nullptr || !_audio->engine()->isInitialized())) {
        // Play session teardown may have shut subsystems down.
        if ((_tickCount % 60) == 0) {
            setStatus(L"Audio engine stopped — reopen Audio Editor to re-init");
        }
        ++_tickCount;
        return;
    }
    if (_driveAudioUpdate && _audio != nullptr) {
        _audio->update(dt);
    }
    ++_tickCount;
    if ((_tickCount % 15) == 0) {
        refreshStatus();
        refreshTracks();
    }
}

bool AudioEditorSession::ensureClips() {
    if (_clipsReady && _clipsAudio == _audio) {
        return true;
    }
    if (_audio == nullptr || _audio->engine() == nullptr) {
        return false;
    }
    _clipA = registerOwnedClip(_pcmA, 440.0f, 0.35f, 0.35f);
    _clipB = registerOwnedClip(_pcmB, 880.0f, 0.25f, 0.30f);
    _clipLoop = registerOwnedClip(_pcmLoop, 220.0f, 1.0f, 0.20f);
    _clipsReady = (_clipA != InvalidClipId && _clipB != InvalidClipId
                   && _clipLoop != InvalidClipId);
    _clipsAudio = _clipsReady ? _audio : nullptr;
    return _clipsReady;
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
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
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

    bindButton("btn_play_440", [this]() { playBeep(_clipA, AudioBus::Sfx); });
    bindButton("btn_play_880", [this]() { playBeep(_clipB, AudioBus::Ui); });
    bindButton("btn_loop_220", [this]() {
        AudioEngine* eng = _audio ? _audio->engine() : nullptr;
        if (eng == nullptr || _clipLoop == InvalidClipId) {
            return;
        }
        if (_loopVoice != InvalidVoice) {
            eng->stop(_loopVoice);
            _loopVoice = InvalidVoice;
        }
        _loopVoice = eng->play(_clipLoop, AudioBus::Music, {}, 0.7f, true);
        refreshStatus();
    });
    bindButton("btn_stop_all", [this]() {
        AudioEngine* eng = _audio ? _audio->engine() : nullptr;
        if (eng != nullptr) {
            eng->stopAll();
        }
        _loopVoice = InvalidVoice;
        stopStream();
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
        if (AudioEngine* eng = _audio ? _audio->engine() : nullptr) {
            eng->setMasterGain(v / 100.0f);
        }
        setLabelValue(valMaster, v, true);
    });
    bindSlider("sld_music", [this, valMusic](float v) {
        if (AudioEngine* eng = _audio ? _audio->engine() : nullptr) {
            eng->setBusGain(AudioBus::Music, v / 100.0f);
        }
        setLabelValue(valMusic, v, true);
    });
    bindSlider("sld_sfx", [this, valSfx](float v) {
        if (AudioEngine* eng = _audio ? _audio->engine() : nullptr) {
            eng->setBusGain(AudioBus::Sfx, v / 100.0f);
        }
        setLabelValue(valSfx, v, true);
    });
    bindSlider("sld_reverb", [this, valReverb](float v) {
        if (AudioEngine* eng = _audio ? _audio->engine() : nullptr) {
            eng->setMasterReverbReturn(v / 100.0f);
        }
        setLabelValue(valReverb, v, true);
    });
    bindSlider("sld_timescale", [this, valTs](float v) {
        const float scale = v / 100.0f;
        if (AudioEngine* eng = _audio ? _audio->engine() : nullptr) {
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
        "btn_stop_stream","btn_spatial_l",     "btn_spatial_c",
        "btn_spatial_r",  "btn_duck",          "btn_reverb_preset",
    };
    for (const char* id : buttonIds) {
        if (auto* b = dynamic_cast<ayt::ui::Button*>(_ui->findById(id))) {
            b->setOnClicked({});
        }
    }
    const char* sliderIds[] = {
        "sld_master", "sld_music", "sld_sfx", "sld_reverb", "sld_timescale",
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
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
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
        std::swprintf(info, 96, L"Music  voice=%u  stream=%u",
                      _stream.voice, _stream.stream);
        setTrackRow(row++, L"BGM Stream", info);
        if (_streamTrackLabel != nullptr) {
            _streamTrackLabel->setText(L"Track: " + pathW);
        }
    } else if (_streamTrackLabel != nullptr) {
        _streamTrackLabel->setText(L"Track: (none)");
    }

    if (eng != nullptr) {
        for (uint32_t i = 0; row < 4; ++i) {
            const VoiceRuntime* vr = eng->voiceAt(i);
            if (vr == nullptr) {
                break;
            }
            if (!vr->playing) {
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
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr) {
        return;
    }
    wchar_t buf[256];
    std::swprintf(buf, 256,
                  L"voices=%u  underruns=%llu  scale=%.2f  duck=%s  reverbP=%u  "
                  L"paused=%s  stream=%s",
                  eng->activeVoiceCount(),
                  static_cast<unsigned long long>(eng->streamUnderrunCount()),
                  eng->timeScale(), _ducked ? L"on" : L"off",
                  static_cast<unsigned>(_reverbPreset),
                  _paused ? L"yes" : L"no",
                  _stream.voice != InvalidVoice ? L"yes" : L"no");
    setStatus(buf);
}

void AudioEditorSession::playBeep(AudioClipId clip, AudioBus bus) {
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr || clip == InvalidClipId) {
        setStatus(L"No engine / clip");
        return;
    }
    (void)eng->play(clip, bus, {}, 1.0f, false);
    refreshStatus();
}

void AudioEditorSession::playSpatial(float x) {
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr || _clipA == InvalidClipId) {
        return;
    }
    SpatializationParams sp{};
    sp.mode = SpatializationParams::Mode::World;
    sp.position = {x, 0, -2};
    sp.referenceDistance = 1.0f;
    sp.maxDistance = 40.0f;
    sp.spatialBlend = 1.0f;
    sp.reverbSend = 0.35f;
    (void)eng->play(_clipA, AudioBus::Sfx, sp, 1.0f, false);
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
    const std::string path = _pathPicker();
    if (path.empty()) {
        setStatus(L"Stream cancelled");
        return;
    }
    if (_stream.voice != InvalidVoice) {
        _audio->stopStreaming(_stream);
        _stream = {};
    }
    _stream = _audio->playStreamingFile(path, AudioBus::Music, true, 1.5f);
    if (_stream.voice == InvalidVoice) {
        _streamPath.clear();
        setStatus(L"Stream open failed — check .wav/.mp3/.ogg + path");
        refreshTracks();
        return;
    }
    _streamPath = path;
    AudioEngine* eng = _audio->engine();
    eng->setMasterGain(1.0f);
    eng->setBusGain(AudioBus::Music, 1.0f);
    if (_clipA != InvalidClipId) {
        (void)eng->play(_clipA, AudioBus::Sfx, {}, 0.55f, false);
    }
    setStatus(L"Streaming (+cue beep)");
    refreshStatus();
    refreshTracks();
}

void AudioEditorSession::stopStream() {
    if (_audio != nullptr && _stream.voice != InvalidVoice) {
        _audio->stopStreaming(_stream);
        _stream = {};
    }
    _streamPath.clear();
    refreshStatus();
    refreshTracks();
}

void AudioEditorSession::togglePause() {
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr) {
        return;
    }
    if (_paused) {
        eng->resume();
        _paused = false;
    } else {
        eng->pause();
        _paused = true;
    }
    refreshStatus();
}

void AudioEditorSession::toggleDuck() {
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr) {
        return;
    }
    _ducked = !_ducked;
    if (_ducked) {
        eng->setDucking(-12.0f, 40.0f, 8.0f);
    } else {
        eng->setDucking(0.0f, 40.0f, 8.0f);
    }
    refreshStatus();
}

void AudioEditorSession::cycleReverbPreset() {
    AudioEngine* eng = _audio ? _audio->engine() : nullptr;
    if (eng == nullptr) {
        return;
    }
    _reverbPreset = static_cast<uint8_t>((_reverbPreset + 1) % 2);
    eng->setReverbPreset(_reverbPreset);
    refreshStatus();
}

} // namespace ayt::audio
