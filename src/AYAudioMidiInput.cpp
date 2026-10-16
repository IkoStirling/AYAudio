#include <AYAudio/MidiInput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#endif
namespace ayt::audio {
struct MidiInput::Impl {
  std::array<MidiEvent, 1024> ring{};
  std::atomic<uint64_t> read{0}, write{0}, dropped{0};
  double origin = 0;
  void receive(uint32_t message, double time) {
    MidiEvent event;
    if (!MidiInput::decode(message, time, event))
      return;
    const auto w = write.load(std::memory_order_relaxed),
               r = read.load(std::memory_order_acquire);
    if (w - r >= ring.size()) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ring[w % ring.size()] = event;
    write.store(w + 1, std::memory_order_release);
  }
#ifdef _WIN32
  HMIDIIN handle = nullptr;
  static void CALLBACK callback(HMIDIIN, UINT message, DWORD_PTR user,
                                DWORD_PTR packed, DWORD_PTR millis) {
    if (message != MIM_DATA && message != MIM_MOREDATA)
      return;
    auto &self = *reinterpret_cast<Impl *>(user);
    self.receive(uint32_t(packed),
                 self.origin + double(uint32_t(millis)) * .001);
  }
#endif
};
MidiInput::MidiInput() : _impl(std::make_unique<Impl>()) {}
MidiInput::~MidiInput() { close(); }
bool MidiInput::decode(uint32_t packed, double time, MidiEvent &out) {
  const uint8_t status = uint8_t(packed), a = uint8_t(packed >> 8),
                b = uint8_t(packed >> 16);
  if (a > 127 || b > 127 || !std::isfinite(time))
    return false;
  MidiEvent e;
  e.channel = status & 15;
  e.key = a;
  e.value = b;
  e.time = time;
  switch (status & 0xf0) {
  case 0x80:
    e.kind = MidiEvent::Kind::NoteOff;
    break;
  case 0x90:
    e.kind = b ? MidiEvent::Kind::NoteOn : MidiEvent::Kind::NoteOff;
    break;
  case 0xb0:
    e.kind = MidiEvent::Kind::ControlChange;
    break;
  case 0xe0:
    e.kind = MidiEvent::Kind::PitchBend;
    break;
  default:
    return false;
  }
  out = e;
  return true;
}
std::vector<MidiDevice> MidiInput::enumerateDevices() {
  std::vector<MidiDevice> devices;
#ifdef _WIN32
  for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
      continue;
    const int count = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr,
                                          0, nullptr, nullptr);
    std::string name(size_t(std::max(1, count)), 0);
    WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), count,
                        nullptr, nullptr);
    if (!name.empty())
      name.pop_back();
    devices.push_back({i, std::move(name)});
  }
#endif
  return devices;
}
bool MidiInput::open(uint32_t id, std::string *error) {
#ifdef _WIN32
  close();
  if (id >= midiInGetNumDevs()) {
    if (error)
      *error = "MIDI device is no longer available";
    return false;
  }
  _impl->origin = std::chrono::duration<double>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  MMRESULT code = midiInOpen(&_impl->handle, id,
                             reinterpret_cast<DWORD_PTR>(&Impl::callback),
                             reinterpret_cast<DWORD_PTR>(_impl.get()),
                             CALLBACK_FUNCTION | MIDI_IO_STATUS);
  if (code == MMSYSERR_NOERROR)
    code = midiInStart(_impl->handle);
  if (code != MMSYSERR_NOERROR) {
    close();
    if (error)
      *error = "MIDI input open/start failed: " + std::to_string(code);
    return false;
  }
  if (error)
    error->clear();
  return true;
#else
  if (error)
    *error = "MIDI device backend is currently Windows-only";
  return false;
#endif
}
void MidiInput::close() {
#ifdef _WIN32
  if (_impl->handle) {
    midiInStop(_impl->handle);
    midiInReset(_impl->handle);
    midiInClose(_impl->handle);
    _impl->handle = nullptr;
  }
#endif
  _impl->read.store(0);
  _impl->write.store(0);
  _impl->dropped.store(0);
}
bool MidiInput::isOpen() const {
#ifdef _WIN32
  return _impl->handle != nullptr;
#else
  return false;
#endif
}
bool MidiInput::poll(MidiEvent &out) {
  const auto r = _impl->read.load(std::memory_order_relaxed);
  if (r == _impl->write.load(std::memory_order_acquire))
    return false;
  out = _impl->ring[r % _impl->ring.size()];
  _impl->read.store(r + 1, std::memory_order_release);
  return true;
}
uint64_t MidiInput::droppedEvents() const {
  return _impl->dropped.load(std::memory_order_relaxed);
}
void MidiInputTestAccess::receive(MidiInput &input, uint32_t message,
                                  double time) {
  input._impl->receive(message, time);
}
bool estimateAudioLatency(const std::vector<float> &probe,
                          const std::vector<float> &capture, uint32_t rate,
                          uint32_t minLag, uint32_t maxLag,
                          AudioLatencyEstimate &out, std::string *error) {
  auto fail = [&](const char *text) {
    if (error)
      *error = text;
    return false;
  };
  if (rate < 8000 || rate > 192000 || probe.size() < 16 || minLag > maxLag ||
      uint64_t(maxLag) + probe.size() > capture.size() ||
      (uint64_t(maxLag) - minLag + 1) * probe.size() > 64000000)
    return fail("invalid calibration range or correlation budget");
  for (float v : probe)
    if (!std::isfinite(v))
      return fail("non-finite calibration PCM");
  for (float v : capture)
    if (!std::isfinite(v))
      return fail("non-finite calibration PCM");
  double mean = 0, energy = 0;
  for (float v : probe)
    mean += v;
  mean /= probe.size();
  for (float v : probe)
    energy += (v - mean) * (v - mean);
  if (energy < 1e-8)
    return fail("calibration probe is silent or constant");
  std::vector<double> sums(capture.size() + 1), squares(capture.size() + 1);
  for (size_t i = 0; i < capture.size(); ++i) {
    sums[i + 1] = sums[i] + capture[i];
    squares[i + 1] = squares[i] + double(capture[i]) * capture[i];
  }
  double best = 0;
  uint32_t bestLag = minLag;
  std::vector<double> correlations;
  correlations.reserve(size_t(maxLag - minLag) + 1);
  for (uint32_t lag = minLag; lag <= maxLag; ++lag) {
    const double sum = sums[lag + probe.size()] - sums[lag],
                 variance = squares[lag + probe.size()] - squares[lag] -
                            sum * sum / probe.size();
    double cross = 0;
    for (size_t i = 0; i < probe.size(); ++i)
      cross += (probe[i] - mean) * capture[lag + i];
    const double corr =
        variance > 1e-9 ? std::abs(cross) / std::sqrt(energy * variance) : 0;
    correlations.push_back(corr);
    if (corr > best) {
      best = corr;
      bestLag = lag;
    }
  }
  double rival = 0;
  for (size_t i = 0; i < correlations.size(); ++i)
    if (std::abs(int64_t(i) + minLag - bestLag) > int64_t(rate / 1000))
      rival = std::max(rival, correlations[i]);
  if (best < .6 || rival > best - .05)
    return fail("calibration is too noisy or ambiguous; use one isolated "
                "broadband probe");
  out = {double(bestLag) / rate, std::clamp(best, 0., 1.)};
  if (error)
    error->clear();
  return true;
}
} // namespace ayt::audio
