#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Authoring/preview data, deliberately outside the recommended AYAudio gameplay API.
namespace ayt::audio::chart {
struct Note { int lane=0; double time=0, end=0; bool operator==(const Note&) const=default; };
struct Document {
    std::string audio;
    bool audioRelative=false;
    double bpm=120, offset=0;
    std::vector<Note> notes;
    double snap(double seconds,int division=4) const {
        const double step=60.0/bpm/std::clamp(division,1,16);
        return std::max(0.0,offset+std::round((seconds-offset)/step)*step);
    }
    void validate(double duration=1e9) const {
        if(audio.empty() || !std::isfinite(bpm) || bpm<20 || bpm>1000 ||
           !std::isfinite(offset) || std::abs(offset)>600 || notes.size()>100000)
            throw std::runtime_error("invalid chart settings");
        for(const auto& n:notes)
            if(n.lane<0 || n.lane>=4 || !std::isfinite(n.time) || !std::isfinite(n.end) ||
               n.time<0 || n.time>=duration || n.end<n.time || n.end>duration)
                throw std::runtime_error("invalid note");
        auto sorted=notes;
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.lane==b.lane?a.time<b.time:a.lane<b.lane;});
        std::array<double,4> lastEnd{-1,-1,-1,-1};
        for(const auto& n:sorted) {
            if(n.lane<0 || n.lane>=4 || !std::isfinite(n.time) || !std::isfinite(n.end) ||
               n.time<0 || n.time>=duration || n.end<n.time || n.end>duration || n.time<=lastEnd[n.lane]+.000001)
                throw std::runtime_error("invalid or overlapping notes");
            lastEnd[n.lane]=n.end;
        }
    }
    nlohmann::json json() const {
        validate(); nlohmann::json list=nlohmann::json::array();
        for(const auto& n:notes) list.push_back({{"lane",n.lane},{"timeSeconds",n.time},{"endSeconds",n.end}});
        return {{"schema","ay.audio.chart"},{"version",1},{"lanes",4},{"audio",audio},
                {"audioRelativeToAssets",audioRelative},{"bpm",bpm},{"beatOffsetSeconds",offset},{"notes",list}};
    }
    static Document fromJson(const nlohmann::json& j) {
        if(j.at("schema")!="ay.audio.chart" || j.at("version")!=1 || j.at("lanes")!=4)
            throw std::runtime_error("unsupported chart format");
        Document d; d.audio=j.at("audio").get<std::string>(); d.audioRelative=j.at("audioRelativeToAssets").get<bool>();
        d.bpm=j.at("bpm").get<double>(); d.offset=j.at("beatOffsetSeconds").get<double>();
        const auto& list=j.at("notes"); if(!list.is_array() || list.size()>100000)throw std::runtime_error("invalid note list");
        for(const auto& n:list)d.notes.push_back({n.at("lane").get<int>(),n.at("timeSeconds").get<double>(),n.at("endSeconds").get<double>()});
        d.validate(); return d;
    }
};
inline std::string utf8(const std::filesystem::path& p) {const auto s=p.u8string();return {s.begin(),s.end()};}
inline std::filesystem::path containedPath(const std::string& root,const std::string& logical) {
    namespace fs=std::filesystem;
    if(root.empty() || logical.empty())throw std::runtime_error("choose a project assets folder");
    const auto rel=fs::u8path(logical);
    if(rel.is_absolute() || rel.has_root_name() || rel.has_root_directory())throw std::runtime_error("use a relative asset path");
    for(const auto& part:rel)if(part=="..")throw std::runtime_error("path escapes assets folder");
    const auto base=fs::weakly_canonical(fs::u8path(root));
    const auto target=fs::weakly_canonical(base/rel);
    auto b=base.begin(),t=target.begin();
    for(;b!=base.end();++b,++t)if(t==target.end() || *b!=*t)throw std::runtime_error("path escapes assets folder");
    if(t==target.end())throw std::runtime_error("asset path must name a file");
    return target;
}
inline std::string read(const std::filesystem::path& file) {
    if(std::filesystem::file_size(file)>16*1024*1024)throw std::runtime_error("chart is too large");
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("cannot open chart");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
struct History {
    struct Snapshot {std::vector<Note> notes;double bpm=120,offset=0;};
    std::vector<Snapshot> undo,redo;
    void remember(const Document& d){undo.push_back({d.notes,d.bpm,d.offset});if(undo.size()>100)undo.erase(undo.begin());redo.clear();}
    bool move(Document& d,bool backwards){auto& source=backwards?undo:redo;auto& dest=backwards?redo:undo;if(source.empty())return false;dest.push_back({d.notes,d.bpm,d.offset});d.notes=std::move(source.back().notes);d.bpm=source.back().bpm;d.offset=source.back().offset;source.pop_back();return true;}
};
// Event-time judgement with ordered pending notes; per-frame work visits due notes only.
struct Judge {
    enum State {Pending,Holding,Perfect,Good,Miss};
    struct Feedback {size_t note=0;State result=Pending;double error=0,accuracy=0,time=0;bool release=false;};
    std::vector<State> states,heads;
    std::vector<double> errors,accuracies;
    std::vector<size_t> order;
    std::array<std::vector<size_t>,4> lanes;
    std::array<size_t,4> laneCursor{};
    std::array<int,4> holding{-1,-1,-1,-1};
    size_t due=0;
    std::vector<Feedback> feedback;
    std::array<bool,4> held{};
    int combo=0,maxCombo=0;double lastError=0,judgedAccuracy=0;std::array<int,5> counts{};
    void reset(const Document& d){
        states.assign(d.notes.size(),Pending);heads=states;errors.assign(states.size(),0);accuracies.assign(states.size(),0);
        held.fill(false);holding.fill(-1);laneCursor.fill(0);due=0;combo=maxCombo=0;lastError=judgedAccuracy=0;counts.fill(0);feedback.clear();
        order.resize(states.size());for(size_t i=0;i<order.size();++i)order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return d.notes[a].time<d.notes[b].time;});
        for(auto& lane:lanes)lane.clear();for(auto i:order)lanes[d.notes[i].lane].push_back(i);
    }
    static double accuracy(double error){return std::clamp(1-std::abs(error)/.15,0.0,1.0)*100;}
    static State grade(double error){return std::abs(error)<=.045?Perfect:Good;}
    void notify(size_t i,State result,double error,double now,bool release=false){
        feedback.push_back({i,result,error,result==Miss?0:accuracies[i],now,release});
        if(feedback.size()>8)feedback.erase(feedback.begin());lastError=error;
    }
    void finish(size_t i,State result){if(states[i]>=Perfect)return;states[i]=result;if(result==Miss)accuracies[i]=0;++counts[result];judgedAccuracy+=accuracies[i];
        combo=result==Miss?0:combo+1;maxCombo=std::max(maxCombo,combo);}
    void key(const Document& d,int lane,bool down,double now){
        if(lane<0||lane>=4||held[lane]==down)return;held[lane]=down;
        if(!down){const int h=holding[lane];if(h>=0&&states[h]==Holding){
                const double error=now-d.notes[h].end;errors[h]=error;accuracies[h]=std::min(accuracies[h],accuracy(error));
                const auto result=std::abs(error)>.15?Miss:(heads[h]==Perfect?grade(error):Good);
                finish(h,result);notify(h,result,error,now,true);}holding[lane]=-1;return;}
        auto& indices=lanes[lane];auto& cursor=laneCursor[lane];
        while(cursor<indices.size()&&(states[indices[cursor]]!=Pending||d.notes[indices[cursor]].time<now-.15))++cursor;
        size_t best=states.size();double distance=.150000001;
        for(size_t p=cursor;p<indices.size();++p){const size_t i=indices[p];if(d.notes[i].time>now+.15)break;
            if(states[i]!=Pending)continue;const double diff=std::abs(now-d.notes[i].time);if(diff<distance){distance=diff;best=i;}}
        if(best==states.size())return;
        const double error=now-d.notes[best].time;errors[best]=error;accuracies[best]=accuracy(error);heads[best]=grade(error);
        if(d.notes[best].end>d.notes[best].time){states[best]=Holding;holding[lane]=int(best);notify(best,heads[best],error,now);}
        else{finish(best,heads[best]);notify(best,heads[best],error,now);}
    }
    void update(const Document& d,double now){
        while(due<order.size()&&d.notes[order[due]].time+.15<now){const auto i=order[due++];
            if(states[i]==Pending){errors[i]=.15;finish(i,Miss);notify(i,Miss,.15,now);}}
        for(int lane=0;lane<4;++lane){const int i=holding[lane];if(i>=0&&states[i]==Holding&&now>=d.notes[i].end){
                finish(i,held[lane]?heads[i]:Miss);notify(i,states[i],0,now,true);holding[lane]=-1;}}
    }
    int count(State s)const{if(s>=Perfect)return counts[s];int active=0;for(int i:holding)if(i>=0 && states[i]==Holding)++active;return s==Holding?active:int(states.size())-active-counts[Perfect]-counts[Good]-counts[Miss];}
    double totalAccuracy()const{const int n=counts[Perfect]+counts[Good]+counts[Miss];return n?judgedAccuracy/n:100;}
};
}
