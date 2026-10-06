#include <AYTest.h>
#include "../demo/audio_editor/ChartDocument.h"
#include <limits>
#include "../demo/audio_editor/AudioRhythmAnalysis.h"
using namespace ayt::audio::chart;
TEST_SUITE(AudioChartTests)
TEST_CASE(VersionedChartRoundTripRejectsInvalidAndOverlappingNotes) {
    Document d;d.audio="audio/music.wav";d.audioRelative=true;d.notes={{0,.5,.5},{1,1,2}};
    const auto decoded=Document::fromJson(d.json());CHECK(decoded.notes==d.notes);CHECK(decoded.audioRelative);CHECK(decoded.audio==d.audio);
    auto rejects=[](const Document& value,double duration=10){try{value.validate(duration);return false;}catch(...){return true;}};
    auto next=d;next.notes.push_back({1,1.5,1.5});CHECK(rejects(next));
    next=d;next.notes.push_back({0,.5,.5});CHECK(rejects(next));
    next=d;next.notes[0].lane=4;CHECK(rejects(next));next=d;next.notes[0].time=-1;CHECK(rejects(next));
    next=d;next.notes[1].end=.9;CHECK(rejects(next));CHECK(rejects(d,1.5));CHECK(rejects(d,.5));
    next=d;next.bpm=std::numeric_limits<double>::quiet_NaN();CHECK(rejects(next));next=d;next.offset=std::numeric_limits<double>::infinity();CHECK(rejects(next));
    auto json=d.json();json["lanes"]=7;bool rejected=false;try{Document::fromJson(json);}catch(...){rejected=true;}CHECK(rejected);
}
TEST_CASE(BeatSnappingAndHistoryPreserveHoldDuration) {
    Document d;d.audio="music.wav";d.offset=.1;CHECK(std::abs(d.snap(.26)-.225)<1e-8);CHECK(d.snap(-10)==0);
    History h;h.remember(d);d.notes.push_back({2,1,2});h.remember(d);d.notes[0]={2,1.5,2.5};
    CHECK(h.move(d,true));CHECK(d.notes[0].end==2);CHECK(h.move(d,true));CHECK(d.notes.empty());CHECK_FALSE(h.move(d,true));
    CHECK(h.move(d,false));CHECK(d.notes[0].time==1);h.remember(d);d.notes.push_back({3,3,3});CHECK_FALSE(h.move(d,false));
}
TEST_CASE(TapAndHoldJudgementUsesEventTimeAndIgnoresRepeat) {
    Document d;d.audio="music.wav";d.notes={{0,1,1},{1,1,2},{2,1,1},{3,3,3}};Judge j;j.reset(d);
    j.key(d,0,true,1.02);CHECK(j.states[0]==Judge::Perfect);CHECK(j.combo==1);
    j.key(d,0,true,1.02);CHECK(j.combo==1);j.key(d,0,false,1.04);
    j.key(d,1,true,1.08);CHECK(j.states[1]==Judge::Holding);j.key(d,1,false,2.01);CHECK(j.states[1]==Judge::Good);
    j.update(d,1.16);CHECK(j.states[2]==Judge::Miss);CHECK(j.combo==0);CHECK(j.maxCombo==2);
    j.key(d,3,true,3);CHECK(j.states[3]==Judge::Perfect);CHECK(j.count(Judge::Perfect)==2);
    j.reset(d);j.key(d,1,true,1);j.key(d,1,false,1.6);CHECK(j.states[1]==Judge::Miss);
    j.reset(d);j.key(d,1,true,1);j.update(d,2);CHECK(j.states[1]==Judge::Perfect);
}
TEST_CASE(ProjectPathsRejectTraversalAndAbsoluteReferences) {
    namespace fs=std::filesystem;const auto root=ayt::test::testTmpDir()/"chart_paths";fs::create_directories(root);
    CHECK(containedPath(utf8(root),"audio/charts/test.chart.json")==fs::weakly_canonical(root/"audio/charts/test.chart.json"));
    auto rejects=[&](const char* path){try{containedPath(utf8(root),path);return false;}catch(...){return true;}};
    CHECK(rejects("../escape.chart.json"));CHECK(rejects("audio/../../escape.chart.json"));CHECK(rejects("C:/outside.chart.json"));CHECK(rejects("/outside.chart.json"));CHECK(rejects("."));
}
TEST_CASE(RhythmAnalysisDetectsTempoPhaseAndHandlesSilenceCancellation){
    using namespace ayt::audio;
    for(double bpm:{72.,90.,120.,137.,175.}){
        const int rate=8000;const double phase=.137;std::vector<float> pcm(rate*30);
        for(double t=phase;t<30;t+=60/bpm)for(int i=0;i<100;++i){const size_t f=size_t(t*rate)+i;if(f<pcm.size())pcm[f]=float(std::exp(-i/15.)*std::sin(i*.85));}
        const auto a=rhythm::analyze(pcm.size(),2,rate,[&](size_t f,unsigned c){return c?-pcm[f]:pcm[f];});
        CHECK(a.reliable());CHECK(std::abs(a.bpm-bpm)<.7);CHECK(std::abs(a.offset-phase)<.015);CHECK(a.onsets.size()>20);
    }
    auto quiet=rhythm::analyze(8000*10,1,8000,[](size_t,unsigned){return 0.f;});CHECK_FALSE(quiet.reliable());CHECK(quiet.onsets.empty());
    auto constant=rhythm::analyze(8000*10,1,8000,[](size_t f,unsigned){return float(.2*std::sin(f*.17));});CHECK_FALSE(constant.reliable());
    auto cancel=std::make_shared<std::atomic<bool>>(true);bool cancelled=false;
    try{rhythm::analyze(8000,1,8000,[](size_t,unsigned){return .2f;},cancel);}catch(...){cancelled=true;}CHECK(cancelled);
}
TEST_CASE(RhythmDraftDensitySnapsValidNotesAndTimingHistoryIsReversible){
    ayt::audio::rhythm::Analysis a;for(int i=0;i<80;++i)a.onsets.push_back({.137+i*.125,1});
    Document doc;doc.audio="music.wav";doc.bpm=120;doc.offset=.137;doc.notes={{3,.5,.5}};
    const auto sparse=ayt::audio::rhythm::draft(a,doc,10,0),dense=ayt::audio::rhythm::draft(a,doc,10,2);
    CHECK(dense.size()>sparse.size());CHECK(dense.size()==79); // last onset snaps beyond the duration boundary
    for(const auto& n:dense){CHECK(n.time==n.end);CHECK(std::abs(doc.snap(n.time,4)-n.time)<1e-9);}
    History h;h.remember(doc);const auto original=doc;doc.notes=dense;doc.validate(10);doc.bpm=137;doc.offset=.14;
    CHECK(h.move(doc,true));CHECK(doc.notes==original.notes);CHECK(doc.bpm==120);CHECK(doc.offset==.137);
    CHECK(h.move(doc,false));CHECK(doc.notes==dense);CHECK(doc.bpm==137);CHECK(doc.offset==.14);
}
TEST_CASE(PerNoteAccuracyReportsHeadsTailsMissesAndIgnoresRepeatedKeys){
    Document d;d.audio="music.wav";d.notes={{0,1,1},{1,2,3},{2,4,4}};Judge j;j.reset(d);
    j.key(d,0,true,.97);CHECK(j.feedback.size()==1);CHECK(std::abs(j.accuracies[0]-80)<1e-6);CHECK(j.feedback.back().error<0);
    j.key(d,0,true,.98);CHECK(j.feedback.size()==1);j.key(d,0,false,1.1);
    j.key(d,1,true,2.02);CHECK(j.states[1]==Judge::Holding);CHECK_FALSE(j.feedback.back().release);
    j.key(d,1,false,2.9);CHECK(j.states[1]==Judge::Good);CHECK(j.feedback.back().release);CHECK(std::abs(j.accuracies[1]-100/3.)<1e-6);
    j.update(d,4.2);CHECK(j.states[2]==Judge::Miss);CHECK(j.feedback.back().accuracy==0);CHECK(j.count(Judge::Miss)==1);
    CHECK(std::abs(j.totalAccuracy()-(80+100/3.)/3)<1e-6);
}
TEST_CASE(OrderedJudgeVisitsOnlyDueNotesAndBoundsRecentFeedback){
    Document d;d.audio="music.wav";for(int i=99999;i>=0;--i)d.notes.push_back({i%4,i*.25,i*.25});
    Judge j;j.reset(d);CHECK(j.order.size()==100000);CHECK(j.order.front()==99999);j.update(d,1.2);
    CHECK(j.due==5);CHECK(j.count(Judge::Miss)==5);const auto due=j.due;j.update(d,1.2);CHECK(j.due==due);
    j.update(d,10);CHECK(j.feedback.size()==8);CHECK(j.count(Judge::Miss)==40);
}

TEST_CASE(RhythmDraftKeepsOffGridAttacksRestsAndStrongerNearbyEvents){
    using namespace ayt::audio::rhythm;Analysis a;
    a.onsets={{.713,1,.8f},{1.02,.6f,.2f},{1.027,1.2f,.2f},{1.319,1,.8f},{4.06,1.1f,.2f},{4.37,1,.8f}};
    Document doc;doc.audio="song.wav";doc.bpm=120;
    const auto notes=draft(a,doc,5,2);CHECK(notes.size()==5);CHECK(notes[0].time==.713);CHECK(notes[1].time==1.027);CHECK(notes[2].time==1.319);
    CHECK(notes[3].time==4.06);CHECK(notes[0].lane<2);CHECK(notes[1].lane>=2);
    for(const auto& n:notes){CHECK(n.time==n.end);bool actual=false;for(const auto& o:a.onsets)actual|=n.time==o.time;CHECK(actual);}
    const auto snapped=draft(a,doc,5,2,true);CHECK(snapped[0].time==.713);CHECK(snapped[1].time==1.027);CHECK(snapped[3].time==4.06);
    Analysis nearBeat;nearBeat.onsets={{1.02,1,.8f},{1.31,1,.2f}};const auto nearby=draft(nearBeat,doc,2,2,true);
    CHECK(nearby[0].time==1);CHECK(nearby[1].time==1.31);CHECK(gridFit(a,doc)<.5);
}
TEST_CASE(SpectralAttacksFollowIrregularNotesInsteadOfUniformBeatFilling){
    using namespace ayt::audio::rhythm;const unsigned rate=8000;std::vector<float> pcm(rate*8);
    const std::vector<double> heads{.713,1.017,1.32,2.06,2.37,4.13,4.61,5.91};
    for(size_t n=0;n<heads.size();++n){const size_t first=size_t(heads[n]*rate);const double hz=n%2?880:220;
        for(size_t f=0;f<rate*.18&&first+f<pcm.size();++f)pcm[first+f]+=float(.3*std::exp(-double(f)/(rate*.05))*std::sin(6.283185307179586*hz*f/rate));}
    const auto a=analyze(pcm.size(),1,rate,[&](size_t f,unsigned){return pcm[f];});CHECK(a.onsets.size()>=heads.size());
    for(double head:heads){bool found=false;for(const auto& o:a.onsets)if(std::abs(head-o.time)<.04)found=true;CHECK(found);}
    Document doc;doc.audio="music.wav";doc.bpm=120;const auto notes=draft(a,doc,8,1);CHECK(notes.size()>=6);CHECK(notes.size()<=heads.size()+1);
    for(const auto& n:notes){bool nearHead=false;for(double head:heads)if(std::abs(head-n.time)<.04)nearHead=true;CHECK(nearHead);}
}

TEST_SUITE_END
