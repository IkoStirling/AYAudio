#pragma once

// Shared core for AYAudio_AudioEditor.exe and AYEditor ChildWindow host.
// Mixer / playback desk (Gallery feature set) bound to chrome JSON ids.

#include "AYUI/UIManager.h"

#include <AYAudio/AudioSubSystem.h>

#include <cstdint>
#include <array>
#include <memory>
#include <AYAudio/SampleInstrument.h>
#include <functional>
#include <string>
#include <vector>

namespace ayt::ui {
class Button;
class Slider;
class TextLabel;
}

namespace ayt::audio {

class AudioEditorSession {
public:
    using PathPicker = std::function<std::string()>;

    AudioEditorSession() = default;
    ~AudioEditorSession();

    AudioEditorSession(const AudioEditorSession&) = delete;
    AudioEditorSession& operator=(const AudioEditorSession&) = delete;

    // Non-owning. Editor: GameLoop-registered AudioSubSystem.
    // Standalone: host-owned instance. Call before attach().
    void setAudio(AudioSubSystem* audio) { _audio = audio; }
    AudioSubSystem* audio() const { return _audio; }

    // Returns a filesystem path readable by CreateFileA / ayio::File
    // (ACP on Windows). Empty = cancel.
    void setPathPicker(PathPicker picker) { _pathPicker = std::move(picker); }

    // Standalone hosts drive AudioSubSystem::update from tick().
    // Editor GameLoop already pumps Audio — set false to avoid double update.
    void setDriveAudioUpdate(bool drive) { _driveAudioUpdate = drive; }

    bool attach(ayt::ui::UIManager& ui);
    void detach();

    // Optional audio update + status chrome refresh.
    void tick(float dt);

    // Native virtual-key values (A..Z/Space). Release is processed even while text editing.
    bool handleKey(int key, bool pressed);
    bool handleMouseButton(float x, float y, int button, bool pressed);
    void releaseKeys();
    void setAssetRoot(std::string root) { _assetRoot=std::move(root); }
    bool isAttached() const { return _ui != nullptr; }

private:
    bool ensureClips();
    void releaseResources();
    void buildInstrument(bool sine = false);
    void loadSample();
    void updateKeys();
    void stopOwned();
    void pressNote(int index, bool keyboard, bool down);
    AudioEngine* liveEngine() const;
    std::unique_ptr<SampleInstrument> _instrument;
    AudioEngine* _boundEngine = nullptr;
    uint64_t _engineGeneration = 0;
    std::vector<AudioClipId> _instrumentClips;
    std::vector<VoiceHandle> _ownedVoices;
    std::array<bool,13> _keyboard{}, _mouse{};
    int _mouseKey=-1, _octave=4;
    float _velocity=0.8f, _previewGain=0.7f;
    bool _sine=false, _refreshing=false, _spaceDown=false, _pedalLatch=false;
    std::string _assetRoot;
    std::wstring _instrumentName=L"Piano";
    void bindChrome();
    void unbindChrome();
    void refreshStatus();
    void refreshTracks();
    void setStatus(const std::wstring& text);
    void setTrackRow(int index, const std::wstring& name, const std::wstring& info);

    ayt::ui::Button* bindButton(const char* id, std::function<void()> fn);
    ayt::ui::Slider* bindSlider(const char* id,
                                std::function<void(float)> fn);

    void playBeep(AudioClipId clip, AudioBus bus);
    void playSpatial(float x);
    void openStream();
    void stopStream();
    void togglePause();
    void toggleDuck();
    void cycleReverbPreset();

    static std::vector<float> makeSine(uint32_t sampleRate, uint16_t channels,
                                       float freqHz, float seconds, float amp);
    AudioClipId registerOwnedClip(std::vector<float>& storage, float freqHz,
                                  float seconds, float amp);

    ayt::ui::UIManager* _ui = nullptr;
    AudioSubSystem* _audio = nullptr;
    PathPicker _pathPicker;

    AudioClipId _clipA = InvalidClipId;
    AudioClipId _clipB = InvalidClipId;
    AudioClipId _clipLoop = InvalidClipId;
    VoiceHandle _loopVoice = InvalidVoice;
    StreamingPlayback _stream{};
    std::string _streamPath;

    std::vector<float> _pcmA;
    std::vector<float> _pcmB;
    std::vector<float> _pcmLoop;

    bool _clipsReady = false;
    AudioSubSystem* _clipsAudio = nullptr;
    bool _driveAudioUpdate = true;
    bool _ducked = false;
    uint8_t _reverbPreset = 0;
    bool _paused = false;
    uint32_t _tickCount = 0;

    ayt::ui::TextLabel* _status = nullptr;
    ayt::ui::TextLabel* _streamTrackLabel = nullptr;
};

} // namespace ayt::audio
