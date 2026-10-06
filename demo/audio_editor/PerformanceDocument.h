#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Editor authoring model. Musical positions use integer ticks; device/input time
// is retained separately so changing the grid never destroys the original take.
namespace ayt::audio::performance {
using Tick = int64_t;
inline constexpr Tick PPQ=96000, MaxTick=PPQ*20000;
inline Tick ticks(double beats) {
    if(!std::isfinite(beats) || beats<0 || beats>20000) throw std::runtime_error("invalid beat position");
    return static_cast<Tick>(std::llround(beats*PPQ));
}
inline double beats(Tick t) {return double(t)/PPQ;}
inline Tick snap(Tick t,int division) {const Tick grid=PPQ/std::clamp(division,1,8);return std::max<Tick>(0,((t+grid/2)/grid)*grid);}
struct Instrument {
    std::string kind="piano", source;
    bool relative=false;
    int root=60;
    float gain=.7f,attack=.004f,release=.14f;
    bool operator==(const Instrument&)const=default;
};
struct Note {
    uint64_t id=0;
    int pitch=60;
    Tick start=0,duration=PPQ;
    float velocity=.8f;
    double rawStart=0,rawDuration=1;
    bool operator==(const Note&)const=default;
};
struct Pedal {Tick time=0;bool down=false;bool operator==(const Pedal&)const=default;};
struct Clip {
    uint64_t id=0;
    std::string name="片段";
    Instrument instrument;
    Tick position=0,offset=0,length=4*PPQ,sourceLength=4*PPQ;
    int repeats=1;
    std::vector<Note> notes;
    std::vector<Pedal> pedals;
    bool operator==(const Clip&)const=default;
};
struct Track {
    uint64_t id=0;
    std::string name="钢琴";
    float gain=1;
    bool mute=false,solo=false;
    std::vector<Clip> clips;
    bool operator==(const Track&)const=default;
};
struct Document {
    double bpm=120;
    float master=.8f;
    uint64_t nextId=1;
    std::vector<Track> tracks;
    bool operator==(const Document&)const=default;
    uint64_t allocate(){if(nextId>=1000000000)throw std::runtime_error("identity limit reached");return nextId++;}
    Track& addTrack(){Track t;t.id=allocate();t.name="轨道 "+std::to_string(tracks.size()+1);tracks.push_back(t);return tracks.back();}
    Tick end()const {Tick result=4*PPQ;for(const auto& t:tracks)for(const auto& c:t.clips)result=std::max(result,c.position+c.length*c.repeats);return result;}
    double seconds(Tick t)const{return beats(t)*60/bpm;}
    Tick fromSeconds(double s)const{return ticks(std::max(0.0,s*bpm/60));}
    Clip* clip(uint64_t id){for(auto& t:tracks)for(auto& c:t.clips)if(c.id==id)return &c;return nullptr;}
    const Clip* clip(uint64_t id)const{return const_cast<Document*>(this)->clip(id);}
    Track* track(uint64_t id){for(auto& t:tracks)if(t.id==id)return &t;return nullptr;}
    void validate()const {
        if(!std::isfinite(bpm)||bpm<20||bpm>1000||!std::isfinite(master)||master<0||master>2||tracks.size()>64||nextId==0||nextId>=1000000000)throw std::runtime_error("invalid arrangement settings");
        std::set<uint64_t> ids;size_t count=0,clipCount=0;
        auto identity=[&](uint64_t id){if(!id||id>=nextId||!ids.insert(id).second)throw std::runtime_error("invalid or duplicate identity");};
        for(const auto& t:tracks){identity(t.id);if(t.name.empty()||t.name.size()>256||!std::isfinite(t.gain)||t.gain<0||t.gain>2)throw std::runtime_error("invalid track");
            for(const auto& c:t.clips){identity(c.id);if(++clipCount>4096||c.name.empty()||c.name.size()>256||c.position<0||c.offset<0||c.length<=0||c.sourceLength<=0||c.sourceLength>MaxTick||c.offset>c.sourceLength||c.length>c.sourceLength-c.offset||c.repeats<1||c.repeats>256||c.position>MaxTick-c.length*c.repeats)throw std::runtime_error("invalid clip range");
                const auto& i=c.instrument;if((i.kind!="piano"&&i.kind!="sine"&&i.kind!="sample")||i.source.size()>4096||(i.kind=="sample"&&i.source.empty())||i.root<0||i.root>127||!std::isfinite(i.gain)||i.gain<0||i.gain>2||!std::isfinite(i.attack)||i.attack<0||i.attack>2||!std::isfinite(i.release)||i.release<0||i.release>10)throw std::runtime_error("invalid instrument");
                count+=c.notes.size();if(count>100000||c.pedals.size()>100000)throw std::runtime_error("event limit reached");
                for(const auto& n:c.notes){identity(n.id);if(n.pitch<0||n.pitch>127||n.start<0||n.start>=c.sourceLength||n.duration<=0||n.duration>c.sourceLength-n.start||!std::isfinite(n.velocity)||n.velocity<=0||n.velocity>1||!std::isfinite(n.rawStart)||n.rawStart<0||n.rawStart>20000||!std::isfinite(n.rawDuration)||n.rawDuration<=0||n.rawDuration>20000)throw std::runtime_error("invalid note");}
                Tick last=-1;for(const auto& p:c.pedals){if(p.time<0||p.time>c.sourceLength||p.time<last)throw std::runtime_error("invalid sustain sequence");last=p.time;}
            }
        }
    }
    nlohmann::json json()const {
        validate();nlohmann::json ts=nlohmann::json::array();
        for(const auto& t:tracks){nlohmann::json cs=nlohmann::json::array();for(const auto& c:t.clips){nlohmann::json ns=nlohmann::json::array(),ps=nlohmann::json::array();
            for(const auto& n:c.notes)ns.push_back({{"id",n.id},{"pitch",n.pitch},{"start",n.start},{"duration",n.duration},{"velocity",n.velocity},{"rawStartBeats",n.rawStart},{"rawDurationBeats",n.rawDuration}});
            for(const auto& p:c.pedals)ps.push_back({{"tick",p.time},{"down",p.down}});
            const auto& i=c.instrument;cs.push_back({{"id",c.id},{"name",c.name},{"position",c.position},{"offset",c.offset},{"length",c.length},{"sourceLength",c.sourceLength},{"repeats",c.repeats},{"instrument",{{"kind",i.kind},{"source",i.source},{"relative",i.relative},{"root",i.root},{"gain",i.gain},{"attack",i.attack},{"release",i.release}}},{"notes",ns},{"pedals",ps}});
        }ts.push_back({{"id",t.id},{"name",t.name},{"gain",t.gain},{"mute",t.mute},{"solo",t.solo},{"clips",cs}});}
        return {{"schema","ay.audio.performance"},{"version",1},{"ppq",PPQ},{"bpm",bpm},{"master",master},{"nextId",nextId},{"tracks",ts}};
    }
    static Document fromJson(const nlohmann::json& j){
        if(j.at("schema")!="ay.audio.performance"||j.at("version")!=1||j.at("ppq")!=PPQ)throw std::runtime_error("unsupported performance document");
        Document d;d.bpm=j.at("bpm").get<double>();d.master=j.at("master").get<float>();d.nextId=j.at("nextId").get<uint64_t>();
        const auto& ts=j.at("tracks");if(!ts.is_array()||ts.size()>64)throw std::runtime_error("track limit reached");size_t noteCount=0,clipCount=0;
        for(const auto& x:ts){Track t;t.id=x.at("id").get<uint64_t>();t.name=x.at("name").get<std::string>();t.gain=x.at("gain").get<float>();t.mute=x.at("mute").get<bool>();t.solo=x.at("solo").get<bool>();const auto& cs=x.at("clips");if(!cs.is_array()||(clipCount+=cs.size())>4096)throw std::runtime_error("clip limit reached");
            for(const auto& y:cs){Clip c;c.id=y.at("id").get<uint64_t>();c.name=y.at("name").get<std::string>();c.position=y.at("position").get<Tick>();c.offset=y.at("offset").get<Tick>();c.length=y.at("length").get<Tick>();c.sourceLength=y.at("sourceLength").get<Tick>();c.repeats=y.at("repeats").get<int>();
                const auto& i=y.at("instrument");c.instrument={i.at("kind").get<std::string>(),i.at("source").get<std::string>(),i.at("relative").get<bool>(),i.at("root").get<int>(),i.at("gain").get<float>(),i.at("attack").get<float>(),i.at("release").get<float>()};
                const auto& ns=y.at("notes");const auto& ps=y.at("pedals");if(!ns.is_array()||(noteCount+=ns.size())>100000||!ps.is_array()||ps.size()>100000)throw std::runtime_error("event limit reached");
                for(const auto& n:ns)c.notes.push_back({n.at("id").get<uint64_t>(),n.at("pitch").get<int>(),n.at("start").get<Tick>(),n.at("duration").get<Tick>(),n.at("velocity").get<float>(),n.at("rawStartBeats").get<double>(),n.at("rawDurationBeats").get<double>()});
                for(const auto& p:ps)c.pedals.push_back({p.at("tick").get<Tick>(),p.at("down").get<bool>()});t.clips.push_back(std::move(c));
            }d.tracks.push_back(std::move(t));
        }d.validate();return d;
    }
    Clip& duplicate(uint64_t id,uint64_t trackId,Tick position){auto* old=clip(id);auto* dest=track(trackId);if(!old||!dest)throw std::runtime_error("choose a clip and track");Clip copy=*old;copy.id=allocate();copy.position=position;copy.name+=" 副本";for(auto& n:copy.notes)n.id=allocate();dest->clips.push_back(std::move(copy));return dest->clips.back();}
    void quantize(uint64_t id,int division,bool ends){auto* c=clip(id);if(!c)return;for(auto& n:c->notes){n.start=snap(ticks(n.rawStart),division);const Tick duration=std::max<Tick>(1,ticks(n.rawDuration));n.duration=ends?std::max<Tick>(PPQ/std::clamp(division,1,8),snap(ticks(n.rawStart+n.rawDuration),division)-n.start):duration;c->sourceLength=std::max(c->sourceLength,n.start+n.duration);}c->length=std::max(c->length,c->sourceLength-c->offset);}
};
struct EditState {Document document;Tick cursor=0;uint64_t track=0,clip=0;bool operator==(const EditState&)const=default;};
struct History {
    std::vector<EditState> undo,redo;
    void remember(const EditState& s){undo.push_back(s);if(undo.size()>100)undo.erase(undo.begin());redo.clear();}
    bool move(EditState& s,bool backwards){auto& src=backwards?undo:redo;auto& dst=backwards?redo:undo;if(src.empty())return false;dst.push_back(s);s=std::move(src.back());src.pop_back();return true;}
};
// Real-time take acquisition, called at the input boundary, never from UI dt.
struct Take {
    Clip clip;double bpm=120;int division=4;bool snapEnds=false;
    std::array<int,128> held;bool pedal=false;
    Take(){held.fill(-1);}
    void begin(Document& d,const Instrument& instrument,Tick position,int grid,bool ends,bool sustain=false){clip={};clip.id=d.allocate();clip.instrument=instrument;clip.position=position;clip.name="录制片段";clip.notes.clear();clip.pedals.clear();bpm=d.bpm;division=grid;snapEnds=ends;held.fill(-1);pedal=sustain;if(pedal)clip.pedals.push_back({0,true});}
    void note(Document& d,int pitch,bool down,double seconds,float velocity){if(pitch<0||pitch>127||!std::isfinite(seconds)||seconds<0)return;const double raw=seconds*bpm/60;const Tick at=ticks(raw);
        if(down){if(held[pitch]>=0)return;Note n;n.id=d.allocate();n.pitch=pitch;n.start=snap(at,division);n.duration=1;n.velocity=velocity;n.rawStart=raw;n.rawDuration=1.0/PPQ;held[pitch]=int(clip.notes.size());clip.notes.push_back(n);}
        else if(held[pitch]>=0){auto& n=clip.notes[held[pitch]];n.rawDuration=std::max(1.0/PPQ,raw-n.rawStart);n.duration=snapEnds?std::max<Tick>(PPQ/std::clamp(division,1,8),snap(at,division)-n.start):std::max<Tick>(1,ticks(n.rawDuration));held[pitch]=-1;}
    }
    void sustain(bool down,double seconds){if(down==pedal||seconds<0)return;pedal=down;clip.pedals.push_back({ticks(seconds*bpm/60),down});}
    void finish(Document& d,double seconds){for(int p=0;p<128;++p)if(held[p]>=0)note(d,p,false,seconds,1);if(pedal)sustain(false,seconds);Tick end=ticks(std::max(0.0,seconds)*bpm/60);for(const auto& n:clip.notes)end=std::max(end,n.start+n.duration);for(const auto& p:clip.pedals)end=std::max(end,p.time);const Tick grid=PPQ/std::clamp(division,1,8);clip.sourceLength=clip.length=std::max(grid,((end+grid-1)/grid)*grid);}
};
struct Step {
    std::array<bool,128> held{};bool pending=false;EditState before;Tick duration=PPQ;
    bool note(EditState& s,const Instrument& instrument,int pitch,bool down,Tick length,float velocity,History& history){if(pitch<0||pitch>127||held[pitch]==down)return false;held[pitch]=down;
        if(down){if(!pending){before=s;duration=length;pending=true;auto* t=s.document.track(s.track);if(!t){auto& next=s.document.addTrack();s.track=next.id;}if(!s.document.clip(s.clip)){Clip c;c.id=s.document.allocate();c.name="步进片段";c.position=s.cursor;c.instrument=instrument;s.document.track(s.track)->clips.push_back(c);s.clip=c.id;}}
            auto* c=s.document.clip(s.clip);const Tick start=std::max<Tick>(0,s.cursor-c->position+c->offset);Note n{s.document.allocate(),pitch,start,duration,velocity,beats(start),beats(duration)};c->notes.push_back(n);c->sourceLength=std::max(c->sourceLength,start+duration);c->length=std::max(c->length,c->sourceLength-c->offset);return true;
        }
        if(pending&&std::none_of(held.begin(),held.end(),[](bool b){return b;})){history.remember(before);s.cursor+=duration;pending=false;return true;}return false;
    }
    void finish(EditState& s,History& h){if(pending){h.remember(before);s.cursor+=duration;}held.fill(false);pending=false;}
};
}
