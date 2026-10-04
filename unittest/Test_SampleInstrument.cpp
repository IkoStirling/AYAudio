#include <AYAudio.h>
#include <AYTest.h>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>
using namespace ayt::audio;
namespace {
AudioSettings settings(){AudioSettings s;s.sampleRate=1000;s.maxVoices=8;s.reverbReturn=0;return s;}
AudioClipId clip(AudioEngine& e){std::vector<float> pcm(10000,0.25f);AudioClipDesc d;d.sampleRate=1000;d.channels=1;d.frameCount=static_cast<uint32_t>(pcm.size());d.pcmData=pcm.data();return e.registerClip(d);}
}
TEST_SUITE(SampleInstrumentTests)
TEST_CASE(StaleHandlesCannotStopOrEditReusedSlot) {
    AudioEngine e;CHECK(e.initialize(settings()));const auto c=clip(e);
    const auto old=e.play(c);e.stop(old);const auto fresh=e.play(c);CHECK(old!=fresh);
    e.stop(old);e.setVoiceVolume(old,0);e.setVoicePitch(old,4);e.releaseVoice(old);e.setVoicePaused(old,true);CHECK(!e.setVoicePositionFrames(old,20));
    e.submitFrame(0);VoiceRuntime v;CHECK(e.voiceState(fresh,v));CHECK(v.volume==1);CHECK(v.pitch==1);CHECK(!v.paused);
    e.shutdown();CHECK(e.initialize(settings()));const auto again=e.play(clip(e));CHECK(again!=fresh);e.stop(fresh);CHECK(e.voiceState(again,v));
}
TEST_CASE(RegistrationReleaseReuseCannotReplayFreedPCM) {
    AudioEngine e;CHECK(e.initialize(settings()));const auto c=clip(e);e.releaseClip(c);const auto c2=clip(e);const auto v=e.play(c2);e.submitFrame(0);
    float out[16]{};e.render(out,8);VoiceRuntime state;CHECK(e.voiceState(v,state));CHECK(out[0]>0);
}
TEST_CASE(ChordSustainAndReleaseAreAudioFrameDriven) {
    AudioEngine e;CHECK(e.initialize(settings()));const auto c=clip(e);SampleInstrument piano(e,4);CHECK(piano.addRegion({c,60}));piano.setEnvelope(0,0.01f);
    CHECK(piano.noteOn(60)!=0);CHECK(piano.noteOn(64)!=0);CHECK(piano.noteOn(67)!=0);CHECK(piano.activeNotes()==3);
    piano.setSustain(true);piano.noteOff(60);float out[40]{};e.render(out,20);CHECK(piano.activeNotes()==3);
    piano.setSustain(false);e.render(out,5);CHECK(piano.activeNotes()==3);e.render(out,5);CHECK(piano.activeNotes()==2);
    piano.allNotesOff();CHECK(e.activeVoiceCount()==0);
}
TEST_CASE(VoiceStealingAndPauseAffectOnlyInstrument) {
    AudioEngine e;CHECK(e.initialize(settings()));const auto c=clip(e);const auto foreign=e.play(c);SampleInstrument piano(e,2);CHECK(piano.addRegion({c,60}));
    const auto first=piano.noteOn(60);CHECK(piano.noteOn(62)!=0);CHECK(piano.noteOn(64)!=0);VoiceRuntime v;CHECK(!e.voiceState(first,v));CHECK(e.voiceState(foreign,v));
    piano.setPaused(true);float out[20]{};e.render(out,10);CHECK(e.voicePositionFrames(foreign)==10);piano.allNotesOff();CHECK(e.activeVoiceCount()==1);
}
TEST_CASE(ReleaseRampAndRangeStopAtExactFrame) {
    AudioEngine e;CHECK(e.initialize(settings()));const auto c=clip(e);const auto v=e.play(c);CHECK(e.setVoicePlaybackRange(v,20,25));float out[16]{};e.render(out,8);
    VoiceRuntime state;CHECK(!e.voiceState(v,state));CHECK(out[8]>0);CHECK(out[10]==0);
    const auto r=e.play(c);e.releaseVoice(r,0.004f);e.render(out,4);CHECK(out[0]>out[2]);CHECK(out[2]>out[4]);CHECK(out[4]>out[6]);CHECK(!e.voiceState(r,state));
}
TEST_CASE(FractionalPitchAndAttackDoNotDependOnBlockPartition) {
    AudioEngine a,b;CHECK(a.initialize(settings()));CHECK(b.initialize(settings()));
    const auto va=a.play(clip(a)),vb=b.play(clip(b));a.setVoicePitch(va,0.75f);b.setVoicePitch(vb,0.75f);a.setVoiceEnvelope(va,0.008f,0);b.setVoiceEnvelope(vb,0.008f,0);
    float one[20]{},parts[20]{};a.render(one,10);b.render(parts,3);b.render(parts+6,7);for(int i=0;i<20;++i) CHECK(std::abs(one[i]-parts[i])<1e-6f);CHECK(a.voicePositionFrames(va)==b.voicePositionFrames(vb));
}
TEST_CASE(InvalidRegionsVelocityAndRangesFailSafely) {
    AudioEngine e;CHECK(e.initialize(settings()));SampleInstrument piano(e);CHECK(!piano.addRegion({0,60}));const auto c=clip(e);CHECK(piano.addRegion({c,60,55,65}));
    CHECK(piano.noteOn(90)==0);CHECK(piano.noteOn(60,0)==0);CHECK(piano.noteOn(60,std::numeric_limits<float>::quiet_NaN())==0);
    const auto v=piano.noteOn(60);CHECK(!e.setVoicePlaybackRange(v,20,20));CHECK(!e.setVoicePlaybackRange(v,20000,30000));
}
TEST_CASE(RenderAndClipLifecycleMayOverlapWithoutFreedStorage) {
    AudioEngine e;CHECK(e.initialize(settings()));
    std::thread rendering([&](){float out[64]{};for(int i=0;i<300;++i)e.render(out,32);});
    for(int i=0;i<100;++i){const auto c=clip(e);const auto v=e.play(c);e.releaseVoice(v,0.001f);e.releaseClip(c);}rendering.join();CHECK(e.activeVoiceCount()==0);
}
TEST_CASE(SeekContinuesClipFadeAtSourcePosition) {
    AudioEngine continuous,seeked;CHECK(continuous.initialize(settings()));CHECK(seeked.initialize(settings()));
    const auto a=continuous.play(clip(continuous)),b=seeked.play(clip(seeked));
    continuous.setVoiceEnvelope(a,0.008f,0);seeked.setVoiceEnvelope(b,0.008f,0);
    CHECK(seeked.setVoicePositionFrames(b,5));float reference[12]{},output[2]{};
    continuous.render(reference,6);seeked.render(output,1);
    CHECK(std::abs(reference[10]-output[0])<1e-6f);CHECK(std::abs(reference[11]-output[1])<1e-6f);
}
TEST_CASE(ReleaseBeforeFirstAudioBlockDoesNotClick) {
    AudioEngine e;CHECK(e.initialize(settings()));SampleInstrument instrument(e);
    CHECK(instrument.addRegion({clip(e),60}));instrument.setEnvelope(0.004f,0.004f);
    CHECK(instrument.noteOn(60)!=InvalidVoice);instrument.noteOff(60);
    float output[16]{};e.render(output,8);
    for(float sample:output) CHECK(sample==0);CHECK(instrument.activeNotes()==0);
}
TEST_SUITE_END
