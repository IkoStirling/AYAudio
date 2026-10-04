#include <AYTest.h>
#include "../demo/audio_editor/ChartDocument.h"
#include <limits>
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
TEST_SUITE_END
