#include <AYTest.h>
#include "../demo/audio_editor/PerformanceRenderer.h"
#include <limits>
#include <fstream>
using namespace ayt::audio::performance;
namespace {
bool invalid(const Document& d){try{d.validate();return false;}catch(...){return true;}}
Document fixture(){Document d;d.master=1;auto& t=d.addTrack();Clip c;c.id=d.allocate();c.instrument.kind="sine";c.instrument.gain=1;c.instrument.attack=0;c.instrument.release=0;c.notes.push_back({d.allocate(),60,PPQ/2,PPQ/4,1,.5,.25});t.clips.push_back(c);return d;}
std::string constantSample(){const auto path=ayt::test::testTmpDir()/"performance_constant.wav";RenderResult r;r.rate=8000;r.pcm.assign(8000*2,.4f);const auto bytes=wav(r);std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return ayt::audio::chart::utf8(path);}
void useSample(Document& d,const std::string& source){for(auto& t:d.tracks)for(auto& c:t.clips){c.instrument.kind="sample";c.instrument.source=source;c.instrument.root=60;}}
}
TEST_SUITE(AudioPerformanceTests)
TEST_CASE(BeatGridAndVersionedArrangementPreserveRawTiming){
    CHECK(ticks(.5)==PPQ/2);CHECK(ticks(.25)==PPQ/4);CHECK(ticks(.125)==PPQ/8);CHECK(snap(ticks(.26),4)==PPQ/4);
    auto d=fixture();d.tracks[0].clips[0].notes[0].rawStart=.53;
    const auto decoded=Document::fromJson(d.json());CHECK(decoded==d);CHECK(d.seconds(PPQ/8)==.0625);
    auto bad=d;bad.tracks[0].clips[0].notes[0].duration=0;CHECK(invalid(bad));bad=d;bad.tracks[0].clips[0].notes[0].pitch=128;CHECK(invalid(bad));
    bad=d;bad.bpm=std::numeric_limits<double>::quiet_NaN();CHECK(invalid(bad));bad=d;bad.tracks[0].gain=std::numeric_limits<float>::infinity();CHECK(invalid(bad));
    bad=d;bad.tracks[0].clips[0].offset=bad.tracks[0].clips[0].sourceLength;CHECK(invalid(bad));bad=d;bad.tracks[0].clips[0].repeats=257;CHECK(invalid(bad));
    bad=d;bad.tracks[0].clips[0].notes[0].id=bad.tracks[0].id;CHECK(invalid(bad));
    auto j=d.json();j["version"]=2;bool rejected=false;try{Document::fromJson(j);}catch(...){rejected=true;}CHECK(rejected);
}
TEST_CASE(RealTimeTakeQuantizesOnlyHeadAndPreservesPhysicalDuration){
    Document d;Take take;Instrument i;take.begin(d,i,PPQ*4,4,false);
    take.note(d,60,true,.14,.65f);take.note(d,60,true,.15,.65f);take.note(d,60,false,.33,1);
    take.sustain(true,.25);take.sustain(false,.7);take.finish(d,.8);
    CHECK(take.clip.notes.size()==1);const auto& n=take.clip.notes[0];CHECK(n.start==PPQ/4);CHECK(std::abs(beats(n.duration)-.38)<.00002);CHECK(n.velocity==.65f);CHECK(std::abs(n.rawStart-.28)<1e-8);CHECK(std::abs(n.rawDuration-.38)<1e-8);
    CHECK(take.clip.position==4*PPQ);CHECK(take.clip.pedals.size()==2);CHECK(take.clip.sourceLength>=n.start+n.duration);
    d.addTrack().clips.push_back(take.clip);CHECK_FALSE(invalid(d));
    const auto id=d.tracks[0].clips[0].id;d.quantize(id,8,false);CHECK(d.clip(id)->notes[0].start==PPQ/4);CHECK(std::abs(beats(d.clip(id)->notes[0].duration)-.38)<.00002);
}
TEST_CASE(StopClosesUnreleasedKeysAndSustainAndAvoidsZeroLengthNotes){
    Document d;Take take;take.begin(d,{},0,8,true,true);take.note(d,72,true,0,.8f);take.finish(d,.001);
    CHECK(take.held[72]==-1);CHECK(take.clip.notes[0].duration==PPQ/8);CHECK(take.clip.pedals.size()==2);CHECK_FALSE(take.clip.pedals.back().down);
    d.addTrack().clips.push_back(take.clip);CHECK_FALSE(invalid(d));
}
TEST_CASE(StepChordUndoRedoRestoresCursorAndNewEditsDiscardRedo){
    EditState s;s.track=s.document.addTrack().id;s.cursor=4*PPQ;Step step;History history;
    CHECK(step.note(s,{},60,true,PPQ/2,.8f,history));CHECK(step.note(s,{},64,true,PPQ/2,.7f,history));CHECK_FALSE(step.note(s,{},60,true,PPQ/2,.8f,history));
    CHECK_FALSE(step.note(s,{},60,false,PPQ/2,.8f,history));CHECK(s.cursor==4*PPQ);CHECK(step.note(s,{},64,false,PPQ/2,.7f,history));
    CHECK(s.cursor==4*PPQ+PPQ/2);CHECK(s.document.clip(s.clip)->notes.size()==2);CHECK(s.document.clip(s.clip)->notes[0].duration==PPQ/2);CHECK(history.undo.size()==1);
    const auto recorded=s;CHECK(history.move(s,true));CHECK(s.cursor==4*PPQ);CHECK(s.document.tracks[0].clips.empty());CHECK(history.move(s,false));CHECK(s==recorded);
    CHECK(history.move(s,true));step.note(s,{},67,true,PPQ/8,1,history);step.note(s,{},67,false,PPQ/8,1,history);CHECK_FALSE(history.move(s,false));CHECK(s.cursor==4*PPQ+PPQ/8);
}
TEST_CASE(ClipCopiesAreIndependentAndHistoryRestoresTrackPlacement){
    EditState s;s.document=fixture();s.track=s.document.tracks[0].id;s.clip=s.document.tracks[0].clips[0].id;History h;h.remember(s);
    const auto original=s.clip;const auto copy=s.document.duplicate(original,s.track,4*PPQ).id;s.document.clip(copy)->notes[0].pitch=67;
    CHECK(s.document.clip(original)->notes[0].pitch==60);CHECK(s.document.clip(copy)->notes[0].id!=s.document.clip(original)->notes[0].id);CHECK_FALSE(invalid(s.document));
    CHECK(h.move(s,true));CHECK(s.document.tracks[0].clips.size()==1);CHECK(h.move(s,false));CHECK(s.document.tracks[0].clips.size()==2);
}
TEST_CASE(RenderPlacesHeadsOnSampleFramesAndSamePitchVoicesReleaseIndependently){
    const auto source=constantSample();auto d=fixture();useSample(d,source);auto& c=d.tracks[0].clips[0];c.notes[0].start=PPQ/2;c.notes[0].duration=PPQ/4;
    auto longer=c.notes[0];longer.id=d.allocate();longer.duration=PPQ;c.notes.push_back(longer);
    RenderOptions o;o.rate=8000;o.end=2;const auto result=render(d,o);
    CHECK(result.pcm[1999*2]==0);CHECK(std::abs(result.pcm[2000*2]-.8f)<.0001);CHECK(std::abs(result.pcm[2999*2]-.8f)<.0001);CHECK(std::abs(result.pcm[3000*2]-.4f)<.0001);CHECK(result.pcm[6000*2]==0);
    CHECK(result.pcm.size()==16000);CHECK(result.peak>.79f);
}
TEST_CASE(RenderHonorsMuteSoloGainTrimAndLoopsWithoutTouchingDocument){
    auto d=fixture();useSample(d,constantSample());d.tracks[0].clips[0].notes[0].start=0;d.tracks[0].clips[0].notes[0].duration=2*PPQ;
    const auto id=d.tracks[0].clips[0].id;auto& c=*d.clip(id);c.offset=PPQ/2;c.length=PPQ/2;c.repeats=2;c.position=PPQ;
    RenderOptions o;o.rate=8000;o.end=3;const auto original=d;auto result=render(d,o);
    CHECK(result.pcm[3999*2]==0);CHECK(std::abs(result.pcm[4000*2]-.4f)<.0001);CHECK(std::abs(result.pcm[7999*2]-.4f)<.0001);CHECK(result.pcm[8000*2]==0);CHECK(d==original);
    d.tracks[0].gain=.5f;result=render(d,o);CHECK(std::abs(result.pcm[4000*2]-.2f)<.0001);
    d.tracks[0].mute=true;result=render(d,o);CHECK(result.peak==0);d.tracks[0].mute=false;
    auto& silent=d.addTrack();silent.solo=true;result=render(d,o);CHECK(result.peak==0);
}
TEST_CASE(SustainTailAndRangeExportShareExactPCMAndExcludeMetronome){
    auto d=fixture();useSample(d,constantSample());auto& c=d.tracks[0].clips[0];c.notes[0].start=0;c.notes[0].duration=PPQ/4;c.instrument.release=.05f;c.pedals={{0,true},{PPQ,false}};
    RenderOptions o;o.rate=8000;o.end=2;auto result=render(d,o);CHECK(std::abs(result.pcm[3000*2]-.4f)<.0001);CHECK(result.pcm[4200*2]>0);CHECK(result.pcm[4401*2]==0);
    const auto bytes=wav(result);CHECK(bytes.size()==44+result.pcm.size()*2);CHECK(bytes[20]==1);CHECK(bytes[22]==2);
    Document silent;silent.addTrack();o.tail=false;o.begin=-4;o.end=0;auto noClick=render(silent,o);CHECK(noClick.peak==0);o.metronome=true;const auto clicks=render(silent,o);CHECK(clicks.peak>0);
    o.cancel=std::make_shared<std::atomic<bool>>(true);bool rejected=false;try{render(d,o);}catch(...){rejected=true;}CHECK(rejected);
}

TEST_CASE(EndSnapRoundsToSelectedGridAndShortNotesRemainOneWholeGridCell){
    for(int division:{1,2,4,8}){
        Document d;Take take;take.begin(d,{},0,division,true);const double cell=1.0/division;
        take.note(d,60,true,cell*.2*60/d.bpm,.8f);take.note(d,60,false,cell*.4*60/d.bpm,1);
        take.note(d,64,true,cell*1.6*60/d.bpm,.8f);take.note(d,64,false,cell*3.51*60/d.bpm,1);take.finish(d,cell*4*60/d.bpm);
        const Tick grid=PPQ/division;
        CHECK(take.clip.notes[0].start==0);CHECK(take.clip.notes[0].duration==grid);
        CHECK(take.clip.notes[1].start==2*grid);CHECK(take.clip.notes[1].start+take.clip.notes[1].duration==4*grid);
        d.addTrack().clips.push_back(take.clip);d.quantize(take.clip.id,division,true);
        for(const auto& n:d.clip(take.clip.id)->notes){CHECK(n.start%grid==0);CHECK((n.start+n.duration)%grid==0);CHECK(n.duration>=grid);}
    }
}
TEST_SUITE_END
