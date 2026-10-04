#pragma once
#include <AYAudio/AudioEngine.h>
#include <vector>

namespace ayt::audio {
/// Borrowed registered sample; clip must outlive the instrument's voices.
struct InstrumentRegion {
    AudioClipId clip = InvalidClipId;
    int rootNote = 60;
    int lowNote = 0, highNote = 127;
    float lowVelocity = 0.0f, highVelocity = 1.0f;
    float gain = 1.0f;
    bool loop = false;
};
/// Game-thread sampler. Hosts translate keyboard, pointer or gameplay input into notes.
/// PCM is preloaded through AYResource; noteOn performs no decoding/file I/O.
/// clear/allNotesOff stop only this instrument's voices. No global engine changes.
class SampleInstrument {
public:
    explicit SampleInstrument(AudioEngine& engine, uint32_t polyphony = 16);
    ~SampleInstrument();
    SampleInstrument(const SampleInstrument&) = delete;
    SampleInstrument& operator=(const SampleInstrument&) = delete;
    bool addRegion(const InstrumentRegion& region);
    void clear();
    // Host engine has been destroyed; discard borrowed references without dereferencing.
    void abandon() { _engine=nullptr; _notes.clear(); _regions.clear(); }
    /// Starts a preloaded sample at MIDI pitch and normalized velocity; returns InvalidVoice on failure.
    VoiceHandle noteOn(int note, float velocity = 0.8f);
    void noteOff(int note);
    void setSustain(bool down);
    bool sustain() const { return _sustain; }
    void allNotesOff(bool immediate = true);
    void setPaused(bool paused);
    void setGain(float gain);
    void setEnvelope(float attackSeconds, float releaseSeconds);
    uint32_t activeNotes() const;
private:
    struct Note { int pitch; VoiceHandle voice; float velocity, regionGain; bool held = true, released = false; };
    bool valid() const;
    void reap();
    void release(Note& note);
    AudioEngine* _engine;
    uint64_t _generation;
    uint32_t _polyphony;
    std::vector<InstrumentRegion> _regions;
    std::vector<Note> _notes;
    float _gain = 1.0f, _attack = 0.004f, _release = 0.14f;
    bool _sustain = false, _paused = false;
};
}
