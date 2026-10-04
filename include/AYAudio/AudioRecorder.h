#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ayt::audio {
struct RecordingDevice { std::string id, name; bool input = false, isDefault = false; };
struct RecordingConfig {
    std::string inputDevice, outputDevice; // empty means OS default
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    double maxSeconds = 600;
    bool loopback = false, monitor = false;
};
struct RecordingStatus {
    bool recording = false, paused = false, playing = false, limitReached = false;
    uint64_t frames = 0, droppedFrames = 0;
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    float peak = 0;
    double positionSeconds = 0;
};
class AudioRecorderTestAccess;
// Owner-thread controller. Device callbacks only exchange preallocated PCM;
// update/stop must run before reading or saving the accumulated take.
class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();
    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;
    /// Enumerate OS endpoints, including already installed virtual devices.
    bool enumerateDevices(std::vector<RecordingDevice>& out, std::string* error = nullptr);
    /// Start a fresh take from capture or WASAPI loopback. Failure preserves the previous take.
    bool start(const RecordingConfig& config, std::string* error = nullptr);
    void pause(bool paused);
    void update();
    void stop(); // joins device callbacks and drains remaining PCM
    bool play(const std::string& outputDevice = {}, std::string* error = nullptr);
    void stopPlayback();
    bool clear(); // refuses to clear an active capture
    RecordingStatus status() const;
    std::vector<float> waveform(size_t bins = 512) const;
    const std::vector<float>& samples() const;
    /// Save PCM16 WAV to a relative virtual path below the writable project asset root.
    /// Rejects escape/symlink escape, active/empty takes and existing destinations.
    bool saveWav(const std::string& assetRoot, const std::string& virtualPath,
                 std::string* error = nullptr) const;
private:
    friend class AudioRecorderTestAccess;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
// Deterministic device-callback seam, not a gameplay acquisition API.
class AudioRecorderTestAccess {
public:
    static bool beginBuffered(AudioRecorder&, const RecordingConfig&);
    static void receive(AudioRecorder&, const float*, uint32_t frames);
};
} // namespace ayt::audio
