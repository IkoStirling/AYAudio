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

#include <AYCore.h>
#include <AYGameLoop.h>
#include <AYAudioEngine.h>
#include <cstdint>

namespace ayt::audio
{

class AudioSubSystem : public ayt::game::ISubSystem {
public:
    AudioSubSystem();
    ~AudioSubSystem() override;

    const char* getName() const override { return "Audio"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    // Test/diagnostic accessor. nullptr until initialize() succeeds.
    AudioEngine* engine() { return _engine.get(); }

private:
    std::unique_ptr<AudioEngine> _engine;
    ayt::game::SubSystemDescriptor _descriptor{};
};

} // namespace ayt::audio