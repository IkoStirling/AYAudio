#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace ayt::audio {
struct MidiDevice {
  uint32_t id = 0;
  std::string name;
};
struct MidiEvent {
  enum class Kind { NoteOn, NoteOff, ControlChange, PitchBend };
  Kind kind = Kind::NoteOff;
  uint8_t channel = 0, key = 0, value = 0;
  double time = 0; // steady-clock seconds at device timestamp (origin = start)
};
/// Owner-thread WinMM MIDI 1 input. Callback only writes a bounded 1024-event
/// ring. Poll on owner thread. Stop/close joins callbacks; no engine/UI calls
/// on driver threads. Device IDs belong to the current enumeration. SysEx and
/// MIDI 2 UMP are unsupported.
class MidiInput {
public:
  MidiInput();
  ~MidiInput();
  MidiInput(const MidiInput &) = delete;
  MidiInput &operator=(const MidiInput &) = delete;
  static std::vector<MidiDevice> enumerateDevices();
  /// Opens an enumerated Windows short-message input; poll delivers independent
  /// event values.
  bool open(uint32_t id, std::string *error = nullptr);
  void close();
  bool isOpen() const;
  bool poll(MidiEvent &event);
  uint64_t droppedEvents() const;
  /// Decode a packed MIDI 1 short message. Note-on velocity zero becomes
  /// NoteOff.
  static bool decode(uint32_t message, double seconds, MidiEvent &out);

private:
  friend class MidiInputTestAccess;
  struct Impl;
  std::unique_ptr<Impl> _impl;
};
class MidiInputTestAccess {
public:
  static void receive(MidiInput &, uint32_t message, double time);
};
struct AudioLatencyEstimate {
  double seconds = 0, confidence = 0;
};
/// Compare a known mono probe to a recorded mono take. Reports round-trip lag,
/// including capture, output and route buffering; cannot separate one-way
/// hardware delays. Bounded to 64 million correlations. Failure leaves out
/// unchanged.
bool estimateAudioLatency(const std::vector<float> &probe,
                          const std::vector<float> &capture,
                          uint32_t sampleRate, uint32_t minLag, uint32_t maxLag,
                          AudioLatencyEstimate &out,
                          std::string *error = nullptr);
} // namespace ayt::audio
