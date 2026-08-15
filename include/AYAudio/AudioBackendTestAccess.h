#pragma once
// AYAudio/AudioBackendTestAccess.h - Test-only inspection API for the Mock backend
//
// Tests need to read peak amplitude, last-rendered frame count, and a sample
// of the rendered signal out of MockBackend. We expose that surface here so
// production code doesn't pick up the dependency on MockBackend's concrete
// type. Implementation lives in backend/MockBackend.cpp; tests include this
// header and call the functions below.

#include <cstdint>
#include <AYAudio/IAudioBackend.h>

namespace ayt::audio
{

struct MockBackendStats {
    uint32_t renderCallCount = 0;
    uint32_t lastFrameCount  = 0;
    float    peakAmplitude   = 0.0f;
};

// Returns aggregated stats for the most recent render call. Returns zeros
// when `backend` is not a MockBackend.
MockBackendStats inspectMockBackend(IAudioBackend* backend);

// Read a sample from the most recent rendered block. Returns 0 when the
// backend is not a Mock or the index is out of range.
float mockSampleAt(IAudioBackend* backend, uint32_t frame, uint32_t channel);

} // namespace ayt::audio