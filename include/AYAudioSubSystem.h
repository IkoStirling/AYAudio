#pragma once
// AYAudioSubSystem.h - Game-loop subsystem integration for AYAudio
//
// Registered into AYGameLoop on Client builds (see AYApplication/design.md).
// Owns the AudioEngine instance for the lifetime of the application and
// pumps submitFrame() each game tick. When AYApplication sets the
// "no-audio" flag (CLI -no-audio), this subsystem stays inert — the engine
// is initialized with the Null backend so commands still flow through
// without touching hardware.
//
// v0.5 scope: subsystem lifecycle, Null/Mock backend selection, no editor
// host yet (Editor uses the same subsystem with a Null backend until its
// own host loop is wired in v1+).
//
// v1 integration: the host picks a backend via setBackend() *before*
// initialize() runs. AYApplication does this from registerSubSystems() to
// honor the `-no-audio` CLI flag (Null vs Miniaudio). When no backend is
// installed, initialize() falls back to Null (the v0 default).

#include <AYCore.h>
#include <AYGameLoop.h>
#include <AYAudioEngine.h>

#include <IAudioBackend.h>

#include <cstdint>
#include <memory>

namespace ayt::audio
{

class AudioSubSystem : public ayt::game::ISubSystem {
public:
    AudioSubSystem();
    ~AudioSubSystem() override;

    const char* getName() const override { return "Audio"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    // Install the backend to use when initialize() runs. Takes ownership.
    // Has no effect after initialize() has been called.
    void setBackend(std::unique_ptr<IAudioBackend> backend);

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    // Test/diagnostic accessor. nullptr until initialize() succeeds.
    AudioEngine* engine() { return _engine.get(); }

private:
    std::unique_ptr<AudioEngine> _engine;
    std::unique_ptr<IAudioBackend> _backend;
    ayt::game::SubSystemDescriptor _descriptor{};
};

} // namespace ayt::audio