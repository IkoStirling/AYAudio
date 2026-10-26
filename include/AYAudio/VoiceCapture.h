#pragma once

#include <AYAudio/AudioRecorder.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ayt::audio {

struct VoiceCaptureConfig {
    std::string inputDevice; // Empty selects the OS default.
    std::uint32_t sampleRate = 48'000;
    std::uint8_t channels = 1;
};

class VoiceCaptureTestAccess;

/// Live 20 ms microphone blocks without accumulating a recording take.
/// Device callback only writes a preallocated ring; pollFrame runs on its
/// owner thread and can feed VoicePcmWire or another application codec.
class VoiceCapture {
public:
    VoiceCapture();
    ~VoiceCapture();
    VoiceCapture(const VoiceCapture&) = delete;
    VoiceCapture& operator=(const VoiceCapture&) = delete;

    /// Endpoint IDs are session-local and share the AudioRecorder input/N form.
    bool enumerateDevices(std::vector<RecordingDevice>& out,
                          std::string* error = nullptr);
    bool start(const VoiceCaptureConfig& config, std::string* error = nullptr);
    void stop() noexcept;
    bool running() const noexcept;
    /// Local privacy mute drops callback PCM and queued pre-mute PCM.
    void setMuted(bool muted) noexcept;
    bool muted() const noexcept;
    /// Returns one complete block or false, preserving out when insufficient.
    bool pollFrame(std::vector<float>& out);
    std::uint64_t droppedFrames() const noexcept;
    std::uint32_t sampleRate() const noexcept;
    std::uint8_t channels() const noexcept;

private:
    friend class VoiceCaptureTestAccess;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// Deterministic callback seam for tests, never a gameplay capture source.
class VoiceCaptureTestAccess {
public:
    static bool beginBuffered(VoiceCapture&, const VoiceCaptureConfig&);
    static void receive(VoiceCapture&, const float*, std::uint32_t frames);
};

} // namespace ayt::audio
