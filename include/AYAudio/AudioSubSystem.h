#pragma once
// AYAudio/AudioSubSystem.h - Game-loop subsystem integration for AYAudio
//
// Registered into AYGameLoop on Client builds (see AYApplication/design.md).
// Owns the AudioEngine instance for the lifetime of the application and
// pumps submitFrame() each game tick. When AYApplication sets the
// "no-audio" flag (CLI -no-audio), this subsystem stays inert — the engine
// is initialized with the Null backend so commands still flow through
// without touching hardware.
//
// v0.5 scope: subsystem lifecycle, Null/Mock backend selection.
// Editor registers AudioSubSystem (Miniaudio unless `-no-audio`) and hosts
// Tools → Audio Editor via shared AudioEditorSession.
//
// v1 integration: the host picks a backend via setBackend() *before*
// initialize() runs. AYApplication does this from registerSubSystems() to
// honor the `-no-audio` CLI flag (Null vs Miniaudio). When no backend is
// installed, initialize() falls back to Null (the v0 default).
//
// v1.1 resource bridge: registerClipFromResource(IAudio*) lets gameplay
// load .ayaudio through AYResource and register it with the engine in one
// call. Returns InvalidClipId if the asset can't be mapped (unsupported
// bit depth, zero-length PCM, etc.).
//
// v2.2 stream BGM: playStreamingFile opens an AYResource stream decoder and
// pumps decode→resample→streamPush each update until EOS (or loops).

#include <AYCore.h>
#include <AYGameLoop.h>
#include <AYAudio/AudioEngine.h>

#include <AYAudio/IAudioBackend.h>

// Forward-declare AYResource::IAudio so this header doesn't drag the
// AYResource include path through every AYAudio consumer. The .cpp is the
// only TU that needs the full definition.
namespace ayt::resource { class IAudio; }

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ayt::audio
{

struct StreamingPlayback {
    VoiceHandle voice = InvalidVoice;
    AudioStreamId stream = InvalidStreamId;
};

class AudioSubSystem : public ayt::game::ISubSystem {
public:
    AudioSubSystem();
    ~AudioSubSystem() override;

    // Last constructed instance (GameLoop typically owns one). Tests may
    // construct temporaries — findRegistered tracks the most recent live one.
    static AudioSubSystem* findRegistered() { return s_instance; }

    const char* getName() const override { return "Audio"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    // Install the backend to use when initialize() runs. Takes ownership.
    // Has no effect after initialize() has been called.
    void setBackend(std::unique_ptr<IAudioBackend> backend);

    // Register a clip from an AYResource audio asset. Maps IAudio fields
    // (sample rate / channels / bits / sample count / data) to AudioClipDesc
    // and registers it with the engine. Only 16-bit and 32-bit PCM are
    // accepted (matching AudioSampleFormat::S16 / F32); other bit depths
    // return InvalidClipId.
    AudioClipId registerClipFromResource(const ayt::resource::IAudio* audio);

    // Long BGM: open file stream decoder → playStream; pump in update().
    // Supports .wav/.mp3/.ogg (AYResource AudioDecoder). Returns invalid
    // handles on failure.
    StreamingPlayback playStreamingFile(const std::string& path,
                                        AudioBus bus = AudioBus::Music,
                                        bool loop = false,
                                        float volume = 1.0f);

    void stopStreaming(StreamingPlayback playback);

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    // Test/diagnostic accessor. nullptr until initialize() succeeds.
    AudioEngine* engine() { return _engine.get(); }

private:
    struct StreamPump {
        AudioStreamId streamId = InvalidStreamId;
        VoiceHandle voice = InvalidVoice;
        void* decoder = nullptr; // ayt::resource::AudioStreamDecoder*
        std::string path;
        uint32_t sourceRate = 0;
        uint16_t channels = 0;
        bool loop = false;
        bool eosMarked = false;
        uint32_t diagTicks = 0;
        float lastPumpPeak = 0.0f; // peak of most recent pumpStreams fill
        std::vector<float> decodeScratch;
        std::vector<float> pushScratch;
    };

    void pumpStreams();
    void closePump(StreamPump& pump, bool closeEngineStream);

    std::unique_ptr<AudioEngine> _engine;
    std::unique_ptr<IAudioBackend> _backend;
    ayt::game::SubSystemDescriptor _descriptor{};
    std::vector<StreamPump> _streamPumps;

    static AudioSubSystem* s_instance;
};

} // namespace ayt::audio
