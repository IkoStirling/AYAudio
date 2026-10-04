#include <AYAudio/SampleInstrument.h>
#include <algorithm>
#include <cmath>
#include <limits>
namespace ayt::audio {
SampleInstrument::SampleInstrument(AudioEngine& engine, uint32_t polyphony)
    : _engine(&engine), _generation(engine.lifecycleGeneration()), _polyphony(std::clamp(polyphony,1u,64u)) { _notes.reserve(_polyphony); }
SampleInstrument::~SampleInstrument() { allNotesOff(); }
bool SampleInstrument::valid() const { return _engine && _engine->isInitialized() && _generation == _engine->lifecycleGeneration(); }
bool SampleInstrument::addRegion(const InstrumentRegion& r) {
    if (!valid() || !r.clip || !_engine->clipRegistry().find(r.clip) || r.rootNote<0 || r.rootNote>127
        || r.lowNote<0 || r.highNote>127 || r.lowNote>r.highNote || !std::isfinite(r.gain) || r.gain<0
        || !std::isfinite(r.lowVelocity) || !std::isfinite(r.highVelocity)
        || r.lowVelocity<0 || r.highVelocity>1 || r.lowVelocity>r.highVelocity) return false;
    _regions.push_back(r); return true;
}
void SampleInstrument::reap() {
    _notes.erase(std::remove_if(_notes.begin(),_notes.end(),[&](const Note& n) {
        VoiceRuntime v; return !valid() || !_engine->voiceState(n.voice,v);
    }),_notes.end());
}
VoiceHandle SampleInstrument::noteOn(int note,float velocity) {
    if (!valid() || note<0 || note>127 || !std::isfinite(velocity) || velocity<=0 || velocity>1) return InvalidVoice;
    reap(); const InstrumentRegion* region=nullptr; int distance=128;
    for (const auto& r:_regions) {
        const int d=std::abs(note-r.rootNote);
        const float rate=std::exp2((note-r.rootNote)/12.0f);
        if (note>=r.lowNote && note<=r.highNote && velocity>=r.lowVelocity && velocity<=r.highVelocity
            && rate>=0.5f && rate<=4.0f && d<distance) { region=&r; distance=d; }
    }
    if (!region) return InvalidVoice;
    if (_notes.size()>=_polyphony) {
        auto it=std::find_if(_notes.begin(),_notes.end(),[](const Note& n){return !n.held;});
        if (it==_notes.end()) it=_notes.begin();
        _engine->stop(it->voice); _notes.erase(it);
    }
    const auto voice=_engine->play(region->clip,_bus,{},velocity*region->gain*_gain,region->loop);
    if (!voice) return voice;
    _engine->setVoicePitch(voice,std::exp2((note-region->rootNote)/12.0f));
    _engine->setVoiceEnvelope(voice,_attack,0.015f);
    _engine->setVoicePaused(voice,_paused);
    _notes.push_back({note,voice,velocity,region->gain}); return voice;
}
void SampleInstrument::release(Note& n) { if (!n.released) { _engine->releaseVoice(n.voice,_release); n.released=true; } }
void SampleInstrument::noteOff(int note) {
    if (!valid()) return;
    for (auto& n:_notes) if (n.pitch==note && n.held) { n.held=false; if (!_sustain) release(n); }
}
void SampleInstrument::setSustain(bool down) {
    _sustain=down; if (!down && valid()) for (auto& n:_notes) if (!n.held) release(n);
}
void SampleInstrument::allNotesOff(bool immediate) {
    _sustain=false;
    if (valid()) for (auto& n:_notes) { n.held=false; if (immediate) _engine->stop(n.voice); else release(n); }
    if (immediate || !valid()) _notes.clear();
}
void SampleInstrument::clear() { allNotesOff(); _regions.clear(); }
void SampleInstrument::setPaused(bool paused) { _paused=paused; if (valid()) for (auto& n:_notes) _engine->setVoicePaused(n.voice,paused); }
void SampleInstrument::setGain(float gain) {
    if (!std::isfinite(gain)) return; _gain=std::clamp(gain,0.0f,2.0f);
    if (valid()) for (auto& n:_notes) _engine->setVoiceVolume(n.voice,n.velocity*n.regionGain*_gain);
}
void SampleInstrument::setEnvelope(float attack,float release) {
    if (std::isfinite(attack)) _attack=std::clamp(attack,0.0f,2.0f);
    if (std::isfinite(release)) _release=std::clamp(release,0.0f,10.0f);
}
uint32_t SampleInstrument::activeNotes() const {
    uint32_t count=0; if (valid()) for (const auto& n:_notes) {VoiceRuntime v; if (_engine->voiceState(n.voice,v)) ++count;} return count;
}
}
