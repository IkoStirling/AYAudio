#pragma once
#include "PerformanceDocument.h"
#include "ChartDocument.h"
#include <AYResource/Loader/AudioLoader.h>
#include <AYResource/assetsImpl/Audio.h>
#include <atomic>
#include <cstring>
#include <memory>

namespace ayt::audio::performance {
struct Sample {uint32_t rate=48000,channels=1;std::vector<float> pcm;};
struct RenderOptions {
    double begin=0,end=0; // absolute beats; negative begin supplies pre-roll
    bool metronome=false,tail=true;
    uint32_t rate=48000;
    std::string assets,builtinAssets;
    std::shared_ptr<std::atomic<bool>> cancel;
};
struct RenderResult {uint32_t rate=48000;std::vector<float> pcm;float peak=0;};
inline Sample decodeSample(const std::string& path){
    ayt::resource::AudioLoader loader;auto resource=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(std::filesystem::u8path(path).string()));
    if(!resource||!resource->getSampleRate()||!resource->getChannels()||resource->getChannels()>2||(resource->getBitsPerSample()!=16&&resource->getBitsPerSample()!=32))throw std::runtime_error("cannot decode instrument sample: "+path);
    Sample s;s.rate=resource->getSampleRate();s.channels=resource->getChannels();const size_t count=resource->getDataSize()/(resource->getBitsPerSample()/8);
    if(count==0||count>32000000)throw std::runtime_error("instrument sample exceeds size limit");s.pcm.resize(count);
    if(resource->getBitsPerSample()==32)std::memcpy(s.pcm.data(),resource->getData(),count*sizeof(float));
    else for(size_t n=0;n<count;++n){int16_t v;std::memcpy(&v,resource->getData()+n*2,2);s.pcm[n]=v/32768.f;}
    for(float value:s.pcm)if(!std::isfinite(value))throw std::runtime_error("non-finite instrument PCM");return s;
}
inline Sample generated(int root,bool sine,uint32_t rate){
    Sample s;s.rate=rate;s.pcm.resize(rate*3);const double hz=440*std::exp2((root-69)/12.0);
    for(size_t f=0;f<s.pcm.size();++f){const double t=double(f)/rate;double v=std::sin(6.283185307179586*hz*t);if(!sine)v=v*std::exp(-t*1.4)+.35*std::sin(6.283185307179586*hz*2*t)*std::exp(-t*3)+.12*std::sin(6.283185307179586*hz*3*t)*std::exp(-t*5);s.pcm[f]=float(v*.22*std::min(1.0,t/.004)*std::min(1.0,(3-t)/.02));}return s;
}
inline Tick releaseTick(const Clip& c,const Note& n){
    Tick off=n.start+n.duration;bool held=false;for(const auto& p:c.pedals){if(p.time<=off)held=p.down;else if(held&&!p.down)return p.time;else if(!held)break;}
    return held?c.sourceLength:off;
}
// Render an immutable document on a worker. Each note owns its independent
// source read head/envelope. No host-engine state or UI calls are made here.
inline RenderResult render(const Document& d,const RenderOptions& o){
    d.validate();if(o.rate<8000||o.rate>192000||!std::isfinite(o.begin)||o.begin< -8||!std::isfinite(o.end))throw std::runtime_error("invalid render settings");
    const double end=o.end>o.begin?o.end:beats(d.end()),spb=60/d.bpm;
    double tail=0;if(o.tail)for(const auto& t:d.tracks)for(const auto& c:t.clips)tail=std::max(tail,double(c.instrument.release));
    const double seconds=(end-o.begin)*spb+tail;if(seconds<=0||seconds>600)throw std::runtime_error("render range must be within 10 minutes");
    const uint64_t frames=uint64_t(std::ceil(seconds*o.rate));if(frames*2*sizeof(float)>256*1024*1024)throw std::runtime_error("render exceeds 256 MiB PCM budget");
    if(o.cancel&&o.cancel->load())throw std::runtime_error("render cancelled");
    RenderResult result;result.rate=o.rate;result.pcm.assign(size_t(frames)*2,0);std::map<std::string,Sample> samples;
    auto cancelled=[&]{if(o.cancel&&o.cancel->load())throw std::runtime_error("render cancelled");};
    const bool anySolo=std::any_of(d.tracks.begin(),d.tracks.end(),[](const Track& t){return t.solo;});
    uint64_t work=0;
    for(const auto& t:d.tracks){if(t.mute||(anySolo&&!t.solo))continue;for(const auto& c:t.clips){cancelled();const auto& instrument=c.instrument;
        for(int repeat=0;repeat<c.repeats;++repeat){const double base=beats(c.position+c.length*repeat-c.offset),left=beats(c.position+c.length*repeat),right=left+beats(c.length);
            for(const auto& n:c.notes){cancelled();const double start=base+beats(n.start),off=std::min(right,base+beats(releaseTick(c,n))),audibleEnd=off+instrument.release/spb;
                if(start>=right||audibleEnd<=left||start>=end||audibleEnd<=o.begin)continue;
                int root=instrument.root;std::string key;
                if(instrument.kind=="sample"){key=instrument.relative?chart::utf8(chart::containedPath(o.assets,instrument.source)):instrument.source;}
                else{root=std::clamp(int(std::lround(double(n.pitch)/12))*12,24,96);key=instrument.kind+std::to_string(root);}
                const double pitch=std::exp2((n.pitch-root)/12.0);if(pitch<.5||pitch>4)throw std::runtime_error("note is outside the instrument sample range");
                if(!samples.contains(key)){
                    if(instrument.kind=="sample")samples.emplace(key,decodeSample(key));
                    else{Sample sample;bool loaded=false;if(instrument.kind=="piano"&&!o.builtinAssets.empty())for(const char* ext:{".aud",".mp3"}){const auto path=std::filesystem::u8path(o.builtinAssets)/"samples"/("C"+std::to_string(root/12-1)+ext);if(std::filesystem::exists(path)){sample=decodeSample(chart::utf8(path));loaded=true;break;}}
                        if(!loaded)sample=generated(root,instrument.kind=="sine",o.rate);samples.emplace(key,std::move(sample));}
                }
                const auto& sample=samples.at(key);const double sourceFrames=double(sample.pcm.size()/sample.channels);
                const double firstBeat=std::max({start,left,o.begin}),lastBeat=std::min(audibleEnd,end+(o.tail?tail/spb:0));
                const int64_t headFrame=std::llround((start-o.begin)*spb*o.rate),offFrame=std::llround((off-o.begin)*spb*o.rate);
                int64_t first=std::max<int64_t>(0,std::llround((firstBeat-o.begin)*spb*o.rate)),last=std::min<int64_t>(int64_t(frames),std::llround((lastBeat-o.begin)*spb*o.rate));
                if(last>first&&(work+=uint64_t(last-first))>1000000000)throw std::runtime_error("arrangement render complexity limit reached");
                for(int64_t f=first;f<last;++f){if((f&4095)==0)cancelled();const double age=double(f-headFrame)/o.rate,source=age*sample.rate*pitch;if(source<0||source>=sourceFrames)continue;
                    const size_t a=size_t(source),b=std::min(a+1,size_t(sourceFrames)-1);const float fraction=float(source-a);
                    const float attack=instrument.attack>0?float(std::clamp(age/instrument.attack,0.0,1.0)):1;
                    const float release=f<offFrame?1:(instrument.release>0?float(std::clamp(1-double(f-offFrame)/o.rate/instrument.release,0.0,1.0)):0);
                    const float gain=attack*release*instrument.gain*n.velocity*t.gain*d.master;
                    for(int ch=0;ch<2;++ch){const int sc=sample.channels==1?0:ch;const float value=sample.pcm[a*sample.channels+sc]*(1-fraction)+sample.pcm[b*sample.channels+sc]*fraction;result.pcm[size_t(f)*2+ch]+=value*gain;}
                }
            }
        }
    }}
    if(o.metronome)for(int beat=int(std::ceil(o.begin));beat<end;++beat){cancelled();const int64_t begin=std::llround((beat-o.begin)*spb*o.rate);for(uint32_t f=0;f<o.rate/35&&begin+f<int64_t(frames);++f){const double age=double(f)/o.rate;const float click=float(.15*std::sin(6.283185307179586*(beat%4==0?1600:1100)*age)*std::exp(-age*180));for(int ch=0;ch<2;++ch)result.pcm[size_t(begin+f)*2+ch]+=click;}}
    for(float v:result.pcm)result.peak=std::max(result.peak,std::abs(v));return result;
}
inline std::vector<uint8_t> wav(const RenderResult& result){
    if(result.pcm.empty()||result.pcm.size()%2||result.pcm.size()>0x7fffffff/2)throw std::runtime_error("invalid WAV data");
    const uint32_t bytes=uint32_t(result.pcm.size()*2);std::vector<uint8_t> out;out.reserve(bytes+44);
    auto word=[&](uint32_t v,int size){for(int i=0;i<size;++i)out.push_back(uint8_t(v>>(i*8)));};auto text=[&](const char* s){out.insert(out.end(),s,s+4);};
    text("RIFF");word(bytes+36,4);text("WAVE");text("fmt ");word(16,4);word(1,2);word(2,2);word(result.rate,4);word(result.rate*4,4);word(4,2);word(16,2);text("data");word(bytes,4);
    for(float value:result.pcm){if(!std::isfinite(value))throw std::runtime_error("invalid output PCM");const int v=int(std::lround(std::clamp(value,-1.f,1.f)*32767));word(uint16_t(int16_t(v)),2);}return out;
}
}
