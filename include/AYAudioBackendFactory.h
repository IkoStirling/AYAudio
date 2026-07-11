#pragma once
// AYAudioBackendFactory.h - Construct the backend for a given kind
//
// Game code typically goes through AudioSettings.backend which the engine
// honors via a factory. We expose the constructors individually so tests
// and tools can swap backends without going through AudioSettings.

#include <IAudioBackend.h>
#include <AYAudioTypes.h>
#include <memory>

namespace ayt::audio
{

std::unique_ptr<IAudioBackend> makeNullBackend();
std::unique_ptr<IAudioBackend> makeMockBackend();
std::unique_ptr<IAudioBackend> makeMiniaudioBackend();

// Convenience: pick the right backend from an AudioBackendKind.
// Returns a Null backend for unknown kinds so callers always get a valid
// (if silent) pointer.
std::unique_ptr<IAudioBackend> makeBackend(AudioBackendKind kind);

} // namespace ayt::audio