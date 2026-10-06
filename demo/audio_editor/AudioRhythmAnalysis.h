#pragma once
#include "ChartDocument.h"
#include <atomic>
#include <memory>
#include <numeric>

// Shared internal authoring analysis. No model downloads or new gameplay API.
namespace ayt::audio::rhythm {
struct Onset {double time=0;float strength=0;};
struct Analysis {
    double bpm=0,offset=0,confidence=0;
    std::vector<Onset> onsets;
    bool reliable()const{return bpm>0 && confidence>=.32 && onsets.size()>=6;}
};
inline void checkCancelled(const std::shared_ptr<std::atomic<bool>>& cancel){
    if(cancel && cancel->load())throw std::runtime_error("analysis cancelled");
}
// 5ms energy/attack envelope, local adaptive peak picking and normalized lag correlation.
// Global fixed tempo estimate: phase is a beat candidate, not a semantic downbeat detector.
template<class Read>
Analysis analyze(size_t frames,unsigned channels,unsigned rate,Read read,
                 std::shared_ptr<std::atomic<bool>> cancel={}){
    Analysis out;if(!frames||!channels||!rate)return out;
    const size_t hop=std::max<size_t>(1,rate/200),stride=std::max<size_t>(1,rate/4000);
    const double dt=double(hop)/rate;
    const size_t bins=(frames+hop-1)/hop;
    std::vector<double> energy(bins),attack(bins);double previous=0;
    for(size_t b=0;b<bins;++b){
        if(b%1024==0)checkCancelled(cancel);
        double sum=0,diff=0;size_t count=0;
        for(size_t f=b*hop;f<std::min(frames,(b+1)*hop);f+=stride){
            // Mean absolute channels avoids cancelling opposite-polarity stereo.
            double v=0;for(unsigned c=0;c<channels;++c){const double s=read(f,c);if(std::isfinite(s))v+=std::abs(s);}
            v/=channels;sum+=v*v;diff+=std::max(0.0,v-previous);previous=v;++count;
        }
        energy[b]=std::sqrt(sum/std::max<size_t>(1,count));
        attack[b]=diff/std::max<size_t>(1,count);
    }
    std::vector<double> novelty(bins);double peak=0;
    for(size_t b=0;b<bins;++b){
        novelty[b]=std::max(0.0,energy[b]-(b?energy[b-1]:0))+.5*std::max(0.0,attack[b]-(b?attack[b-1]:0));
        peak=std::max(peak,novelty[b]);
    }
    if(peak<1e-5)return out;
    std::vector<double> prefix(bins+1);
    for(size_t b=0;b<bins;++b)prefix[b+1]=prefix[b]+novelty[b];
    for(size_t b=0;b<bins;++b){
        const size_t lo=b>20?b-20:0,hi=std::min(bins,b+21);
        const double threshold=std::max({peak*.035,(prefix[hi]-prefix[lo])/(hi-lo)*2.1,energy[b]*.12});
        if(novelty[b]<threshold || (b&&novelty[b]<novelty[b-1]) || (b+1<bins&&novelty[b]<=novelty[b+1]))continue;
        const Onset next{b*dt,float(novelty[b]/peak)};
        if(!out.onsets.empty()&&next.time-out.onsets.back().time<.06){if(next.strength>out.onsets.back().strength)out.onsets.back()=next;}
        else out.onsets.push_back(next);
    }
    if(out.onsets.size()<4)return out;
    // Bound tempo work to the first 180s; onset extraction still covers the whole supported song.
    const size_t n=std::min(bins,size_t(180/dt));
    auto corr=[&](size_t lag){double dot=0,a=0,b=0;
        for(size_t i=lag;i<n;++i){const double x=novelty[i],y=novelty[i-lag];dot+=x*y;a+=x*x;b+=y*y;}
        return dot/std::max(1e-12,std::sqrt(a*b));};
    const int low=std::max(1,int(std::floor(60/200.0/dt))),high=int(std::ceil(60/60.0/dt));
    std::vector<double> correlations(high+3);double bestScore=-1,period=0;
    for(int lag=low;lag<=high+1;++lag){checkCancelled(cancel);correlations[lag]=corr(lag);}
    for(double bpm=60;bpm<=200;bpm+=.25){checkCancelled(cancel);
        const double candidate=60/bpm,lag=candidate/dt;const int integer=int(lag);
        const double correlation=correlations[integer]*(1-lag+integer)+correlations[integer+1]*(lag-integer);
        // Half-tempo aliases align every second attack only; circular phase agreement disambiguates them.
        double real=0,imaginary=0,weight=0;
        for(const auto& o:out.onsets){if(o.time>60)break;const double angle=o.time/candidate*6.283185307179586;
            real+=std::cos(angle)*o.strength;imaginary+=std::sin(angle)*o.strength;weight+=o.strength;}
        const double coherence=std::hypot(real,imaginary)/std::max(1e-12,weight);
        const double prior=1-.025*std::abs(std::log2(bpm/120));
        const double score=correlation*(.35+.65*coherence)*prior;
        if(score>bestScore){bestScore=score;period=candidate;}}
    if(period==0||bestScore<.1)return out;
    // Refit a coherent onset subsequence to remove bin-rate tempo rounding drift.
    const double first=out.onsets.front().time;double sw=0,sx=0,sy=0,sxx=0,sxy=0;
    for(const auto& o:out.onsets){if(o.time>180)break;const double k=std::round((o.time-first)/period);
        if(std::abs(o.time-first-k*period)>.035)continue;const double w=o.strength;
        sw+=w;sx+=w*k;sy+=w*o.time;sxx+=w*k*k;sxy+=w*k*o.time;}
    if(sw>0 && sw*sxx-sx*sx>1){const double fitted=(sw*sxy-sx*sy)/(sw*sxx-sx*sx);if(std::abs(fitted-period)<dt*1.5)period=fitted;}
    out.bpm=std::round(60/period*100)/100;
    std::array<double,128> phases{};
    for(const auto& o:out.onsets){if(o.time>180)break;const int bin=int(std::floor(std::fmod(o.time,period)/period*128))%128;phases[bin]+=o.strength;}
    int best=0;double weight=-1;
    for(int i=0;i<128;++i){double w=phases[(i+127)%128]+phases[i]+phases[(i+1)%128];if(w>weight){weight=w;best=i;}}
    double sum=0,total=0;const double center=(best+.5)/128*period;
    for(const auto& o:out.onsets){if(o.time>180)break;double phase=std::fmod(o.time,period);phase+=std::round((center-phase)/period)*period;
        if(std::abs(phase-center)<.04){sum+=phase*o.strength;total+=o.strength;}}
    out.offset=total?std::fmod(sum/total+period,period):0;
    out.confidence=std::clamp(bestScore,0.0,1.0);return out;
}
inline std::vector<chart::Note> draft(const Analysis& a,const chart::Document& doc,double duration,int density){
    std::vector<chart::Note> out;const int division=density<=0?1:density==1?2:4;
    const double minimum=60/doc.bpm/division*.85;double last=-1e9;size_t lane=0;
    for(const auto& o:a.onsets){if(o.strength<(density<=0?.25f:density==1?.12f:.06f))continue;
        const double t=doc.snap(o.time,division);if(t>=duration||t-last<minimum)continue;
        // Repeatable alternating hands; musical intent still needs human refinement.
        static constexpr int lanes[]={0,2,1,3,2,0,3,1};out.push_back({lanes[lane++%8],t,t});last=t;
        if(out.size()==100000)break;
    }
    return out;
}
}
