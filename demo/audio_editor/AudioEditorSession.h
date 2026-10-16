#pragma once

// Shared core for AYAudio_AudioEditor.exe and AYEditor ChildWindow host.
// Owns the five-page workbench mounted into audio_editor_root.

#include "AYUI/UIManager.h"

#include <AYAudio/AudioSubSystem.h>

#include <cstdint>
#include <array>
#include <memory>
#include <AYAudio/SampleInstrument.h>
#include <AYAudio/MidiInput.h>
#include <functional>
#include <string>
#include <vector>

namespace ayt::ui {
class Button;
class Slider;
class TextLabel;
}

namespace ayt::audio {

namespace performance { struct EditState; struct Instrument; }
class AudioRecorder;
class AudioEditorSession {
public:
    using PathPicker = std::function<std::string()>;

    AudioEditorSession();
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

    // Native virtual keys, including VK_LSHIFT/VK_LMENU and OEM comma.
    // Each physical key retains its note-on pitch until release, even after temporary transpose.
    bool handleKey(int key, bool pressed);
    bool handleMouseButton(float x, float y, int button, bool pressed);
    bool handleMouseMove(float x, float y);
    // Logical coordinates; continuous wheel input keeps its starting surface.
    bool handleMouseWheel(float x, float y, float deltaY);
    void onFocusLost();
    bool openResource(const std::string& path);
    // Authoring host integration; chart preview is configured before attach.
    bool openChart(const std::string& path);
    bool openPerformance(const std::string& path);
    performance::EditState* performanceState() const;
    bool performanceRecording() const;
    // Input-boundary monotonic clock, injectable for deterministic authoring tests.
    void setPerformanceClock(std::function<double()> clock) { _performanceClock=std::move(clock); }
    double performanceTime() const;
    double inputOffsetSeconds()const { return _inputOffsetSeconds; }
    uint32_t uiModifiers() const;
    const std::string& builtinAudioAssets() const { return _assetRoot; }
    void setChartPreviewFile(std::string path) { _chartPreviewFile=std::move(path); }
    using ChartLauncher=std::function<bool(const std::string&,std::string&)>;
    void setChartLauncher(ChartLauncher launcher) { _chartLauncher=std::move(launcher); }
    ayt::math::FRectangle pianoKeyBounds(int note) const;
    void releaseKeys();
    /// Owner-thread MIDI delivery; velocities and device timestamps feed the performance recorder.
    void handleMidiEvent(const MidiEvent& event);
    void setAssetRoot(std::string root) { _assetRoot=std::move(root); }
    bool isAttached() const { return _ui != nullptr; }
    AudioRecorder* recorder() const;
    using RecordingRootProvider = std::function<std::string()>;
    // Provider returns the writable project Assets root, not packaged/cache assets.
    void setRecordingRootProvider(RecordingRootProvider provider) { _recordingRoot=std::move(provider); }
    void setRecordingSavedCallback(std::function<void(const std::string&)> callback) { _recordingSaved=std::move(callback); }
    void setRecordingFolderPicker(PathPicker picker) { _recordingFolderPicker=std::move(picker); }


private:
    struct Workbench;
    std::unique_ptr<Workbench> _workbench;
    void pressPitch(int pitch, bool keyboard, bool down);
    bool pitchHeld(int pitch) const;
    bool sustainHeld() const;
    double _inputOffsetSeconds=0;
    bool ensureClips();
    void releaseResources();
    void buildInstrument(bool sine = false);
    void loadSample();
    bool loadSamplePath(const std::string& path,int root);
    void updateKeys();
    void stopOwned();
    void pressNote(int index, bool keyboard, bool down);
    int effectiveTranspose() const;
    AudioEngine* liveEngine() const;
    std::unique_ptr<SampleInstrument> _instrument;
    AudioEngine* _boundEngine = nullptr;
    uint64_t _engineGeneration = 0;
    std::vector<AudioClipId> _instrumentClips;
    std::vector<VoiceHandle> _ownedVoices;
    std::array<bool,128> _keyboard{}, _mouse{};
    std::array<std::array<bool,128>,16> _midi{};
    std::array<bool,16> _midiPedal{};
    std::array<int,256> _keyPitches;
    bool _leftShift=false, _leftAlt=false;
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
    std::function<double()> _performanceClock;
    std::string _performanceSampleSource;
    int _performanceSampleRoot=60;
    PathPicker _pathPicker, _recordingFolderPicker;
    std::string _chartPreviewFile;
    ChartLauncher _chartLauncher;
    RecordingRootProvider _recordingRoot;
    std::function<void(const std::string&)> _recordingSaved;

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
