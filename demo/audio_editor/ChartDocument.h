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
    std::vector<std::vector<Note>> undo,redo;
    void remember(const Document& d){undo.push_back(d.notes);if(undo.size()>100)undo.erase(undo.begin());redo.clear();}
    bool move(Document& d,bool backwards){auto& source=backwards?undo:redo;auto& dest=backwards?redo:undo;if(source.empty())return false;dest.push_back(d.notes);d.notes=std::move(source.back());source.pop_back();return true;}
};
// Small deterministic judgement core used by the independent preview only.
struct Judge {
    enum State {Pending,Holding,Perfect,Good,Miss};
    std::vector<State> states,heads;
    std::array<bool,4> held{};
    int combo=0,maxCombo=0; double lastError=0;
    void reset(const Document& d){states.assign(d.notes.size(),Pending);heads=states;held.fill(false);combo=maxCombo=0;lastError=0;}
    void finish(size_t i,State result){states[i]=result;combo=result==Miss?0:combo+1;maxCombo=std::max(maxCombo,combo);}
    static State grade(double error){return std::abs(error)<=.045?Perfect:Good;}
    void key(const Document& d,int lane,bool down,double now) {
        if(lane<0 || lane>=4 || held[lane]==down)return;
        held[lane]=down;
        if(!down){for(size_t i=0;i<states.size();++i)if(states[i]==Holding && d.notes[i].lane==lane){lastError=now-d.notes[i].end;finish(i,std::abs(lastError)>.15?Miss:(heads[i]==Perfect?grade(lastError):Good));}return;}
        size_t best=states.size();double distance=.150000001;
        for(size_t i=0;i<states.size();++i)if(states[i]==Pending && d.notes[i].lane==lane){const double diff=std::abs(now-d.notes[i].time);if(diff<distance){distance=diff;best=i;}}
        if(best==states.size())return;
        lastError=now-d.notes[best].time;heads[best]=grade(lastError);
        if(d.notes[best].end>d.notes[best].time)states[best]=Holding;else finish(best,heads[best]);
    }
    void update(const Document& d,double now){for(size_t i=0;i<states.size();++i){if(states[i]==Pending && now>d.notes[i].time+.15)finish(i,Miss);else if(states[i]==Holding && now>=d.notes[i].end)finish(i,held[d.notes[i].lane]?heads[i]:Miss);}}
    int count(State s)const{return static_cast<int>(std::count(states.begin(),states.end(),s));}
};
}
