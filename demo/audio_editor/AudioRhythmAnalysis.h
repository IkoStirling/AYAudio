#pragma once
#include "ChartDocument.h"
#include <atomic>
#include <complex>
#include <memory>
#include <map>
#include <numeric>

// Internal authoring support. Detect attacks; never fill empty beat cells.
namespace ayt::audio::rhythm {
struct Onset {double time=0;float strength=0,lowFrequency=.5f;};
struct Analysis {
    double bpm=0,offset=0,confidence=0;
    std::vector<Onset> onsets;
    bool reliable()const{return bpm>0 && confidence>=.32 && onsets.size()>=6;}
};
inline void checkCancelled(const std::shared_ptr<std::atomic<bool>>& cancel){
    if(cancel && cancel->load())throw std::runtime_error("analysis cancelled");
}
inline double median(std::vector<double>& values){
    if(values.empty())return 0;const auto middle=values.begin()+values.size()/2;
    std::nth_element(values.begin(),middle,values.end());return *middle;
}
// Fixed-size radix-2 FFT, log-frequency band flux with adaptive whitening.
// Stereo powers are averaged after FFT so opposite-polarity channels cannot cancel.
template<class Read>
Analysis analyze(size_t frames,unsigned channels,unsigned rate,Read read,
                 std::shared_ptr<std::atomic<bool>> cancel={}){
    Analysis out;checkCancelled(cancel);if(!frames||!channels||!rate)return out;
    constexpr size_t fftSize=512,bandCount=32;constexpr double analysisRate=11025,pi=3.141592653589793;
    constexpr size_t hop=110;constexpr double dt=hop/analysisRate;
    const size_t bins=size_t(std::ceil(double(frames)/rate/dt));
    std::array<size_t,fftSize> reverse{};std::array<float,fftSize> window{};
    std::array<float,fftSize/2> rootReal{},rootImaginary{};std::array<int,fftSize/2+1> band{};
    for(size_t i=0;i<fftSize;++i){size_t x=i,r=0;for(int b=0;b<9;++b){r=(r<<1)|(x&1);x>>=1;}reverse[i]=r;window[i]=float(.5-.5*std::cos(2*pi*i/(fftSize-1)));}
    for(size_t i=0;i<rootReal.size();++i){rootReal[i]=float(std::cos(-2*pi*i/fftSize));rootImaginary[i]=float(std::sin(-2*pi*i/fftSize));}
    for(size_t i=0;i<band.size();++i){const double hz=i*analysisRate/fftSize;
        band[i]=(hz>=45&&hz<5000)?std::clamp(int(std::log(hz/45)/std::log(5000./45)*bandCount),0,int(bandCount)-1):-1;}
    std::array<double,bandCount> average{},previous{};
    std::vector<double> flux(bins),rms(bins),low(bins),scratch;
    for(size_t b=0;b<bins;++b){
        if(b%32==0)checkCancelled(cancel);
        std::array<double,bandCount> energy{};double squares=0;
        for(unsigned c=0;c<channels;++c){std::array<float,fftSize> real{},imaginary{};
            for(size_t i=0;i<fftSize;++i){const double sample=(double(b*hop)+double(i)-fftSize/2)*rate/analysisRate;
                float v=0;if(sample>=0&&sample<double(frames)){const size_t a=size_t(sample),next=std::min(frames-1,a+1);
                    const float va=float(read(a,c)),vb=float(read(next,c));v=va+(vb-va)*float(sample-a);if(!std::isfinite(v))v=0;}
                squares+=v*v;real[reverse[i]]=v*window[i];}
            for(size_t length=2;length<=fftSize;length*=2){const size_t half=length/2,stride=fftSize/length;
                for(size_t start=0;start<fftSize;start+=length)for(size_t j=0;j<half;++j){
                    const size_t a=start+j,b=a+half,k=j*stride;const float ar=real[a],ai=imaginary[a];
                    const float vr=real[b]*rootReal[k]-imaginary[b]*rootImaginary[k],vi=real[b]*rootImaginary[k]+imaginary[b]*rootReal[k];
                    real[a]=ar+vr;imaginary[a]=ai+vi;real[b]=ar-vr;imaginary[b]=ai-vi;}}
            for(size_t i=1;i<band.size();++i)if(band[i]>=0)energy[band[i]]+=(real[i]*real[i]+imaginary[i]*imaginary[i])/channels;
        }
        rms[b]=std::sqrt(squares/(fftSize*channels));double total=0,bass=0;
        for(size_t j=0;j<bandCount;++j){average[j]=.98*average[j]+.02*energy[j];
            const double normalized=std::log1p(energy[j]/(average[j]+1e-4)),difference=std::max(0.0,normalized-previous[j]);
            total+=difference;if(j<17)bass+=difference;previous[j]=normalized;}
        flux[b]=total/bandCount;low[b]=total?bass/total:.5;
    }
    // Local median/MAD separates attacks from sustained tone fluctuation; silence is rejected.
    std::vector<double> novelty(bins);
    for(size_t b=0;b<bins;++b){if(b%512==0)checkCancelled(cancel);
        const size_t lo=b>50?b-50:0,hi=std::min(bins,b+51);
        scratch.assign(flux.begin()+lo,flux.begin()+hi);const double baseline=median(scratch);
        for(auto& v:scratch)v=std::abs(v-baseline);const double deviation=median(scratch);
        novelty[b]=std::max(0.0,flux[b]-baseline);
        if(rms[b]<.001||flux[b]<std::max(.035,baseline+2.8*deviation))continue;
        bool peak=true;for(size_t k=b>2?b-2:0;k<std::min(bins,b+3);++k)if(k!=b&&(flux[k]>flux[b]||(k>b&&flux[k]==flux[b])))peak=false;
        if(!peak)continue;const Onset next{b*dt,float(flux[b]),float(low[b])};
        if(!out.onsets.empty()&&next.time-out.onsets.back().time<.09){if(next.strength>out.onsets.back().strength)out.onsets.back()=next;}
        else out.onsets.push_back(next);
    }
    // Robust strength scale: a single loud opening must not suppress the rest of a song.
    scratch.clear();for(const auto& o:out.onsets)scratch.push_back(o.strength);
    if(!scratch.empty()){const size_t percentile=(scratch.size()-1)*9/10;std::nth_element(scratch.begin(),scratch.begin()+percentile,scratch.end());
        const double scale=std::max(.035,scratch[percentile]);for(auto& o:out.onsets)o.strength=float(std::min(2.0,o.strength/scale));}
    if(out.onsets.size()<6)return out;
    const size_t n=std::min(bins,size_t(180/dt));
    auto corr=[&](size_t lag){double dot=0,a=0,b=0;for(size_t i=lag;i<n;++i){const double x=novelty[i],y=novelty[i-lag];dot+=x*y;a+=x*x;b+=y*y;}
        return dot/std::max(1e-12,std::sqrt(a*b));};
    const int lo=int(std::floor(60/200./dt)),hi=int(std::ceil(60/60./dt));std::vector<double> correlations(hi+3);
    for(int lag=lo;lag<=hi+1;++lag){checkCancelled(cancel);correlations[lag]=corr(lag);}
    double best=-1,period=0,bestCorrelation=0;
    for(double bpm=60;bpm<=200;bpm+=.25){checkCancelled(cancel);const double p=60/bpm,lag=p/dt;const int l=int(lag);
        const double correlation=correlations[l]*(l+1-lag)+correlations[l+1]*(lag-l);
        std::complex<double> phase{},halfPhase{};double weight=0;
        for(const auto& o:out.onsets){if(o.time>60)break;const double w=std::min(1.f,o.strength),angle=o.time/p*2*pi;
            phase+=std::polar(w,angle);halfPhase+=std::polar(w,angle*2);weight+=w;}
        // Half-beat accompaniment must not cancel beat evidence.
        const double fundamental=std::abs(phase)/std::max(1e-12,weight),subdivision=std::abs(halfPhase)/std::max(1e-12,weight);
        const double score=correlation*(.55+.3*fundamental+.15*subdivision)*(1-.025*std::abs(std::log2(bpm/120)));
        if(score>best){best=score;period=p;bestCorrelation=correlation;}}
    if(period==0||best<.1)return out;
    // Refine only a coherent opening; a fixed grid cannot represent tempo drift.
    const double first=out.onsets.front().time;double sw=0,sx=0,sy=0,sxx=0,sxy=0;
    for(const auto& o:out.onsets){if(o.time>20)break;const double k=std::round((o.time-first)/period);
        if(std::abs(o.time-first-k*period)>.035)continue;const double w=std::min(1.f,o.strength);
        sw+=w;sx+=w*k;sy+=w*o.time;sxx+=w*k*k;sxy+=w*k*o.time;}
    if(sw>0&&sw*sxx-sx*sx>1){const double fit=(sw*sxy-sx*sy)/(sw*sxx-sx*sx);if(std::abs(fit-period)<dt*1.5)period=fit;}
    out.bpm=std::round(60/period*100)/100;
    // Phase is a half-beat-grid candidate, not a semantic downbeat claim.
    const double half=period/2;std::array<double,128> phases{};
    for(const auto& o:out.onsets){if(o.time>20)break;phases[int(std::floor(std::fmod(o.time,half)/half*128))%128]+=std::min(1.f,o.strength);}
    int bestBin=0;double weight=-1;for(int i=0;i<128;++i){const double w=phases[(i+127)%128]+phases[i]+phases[(i+1)%128];if(w>weight){weight=w;bestBin=i;}}
    double sum=0,total=0;const double center=(bestBin+.5)/128*half;
    for(const auto& o:out.onsets){if(o.time>20)break;double phase=std::fmod(o.time,half);phase+=std::round((center-phase)/half)*half;
        if(std::abs(phase-center)<.035){const double w=std::min(1.f,o.strength);sum+=phase*w;total+=w;}}
    out.offset=total?std::fmod(sum/total+half,half):0;out.confidence=std::clamp(bestCorrelation,0.0,1.0);return out;
}
inline double gridFit(const Analysis& a,const chart::Document& doc){
    size_t count=0,aligned=0;for(const auto& o:a.onsets){if(o.strength<.55f)continue;++count;if(std::abs(doc.snap(o.time,2)-o.time)<=.035)++aligned;}
    return count?double(aligned)/count:0;
}
inline std::vector<chart::Note> draft(const Analysis& a,const chart::Document& doc,double duration,int density,bool nearbySnap=false){
    std::vector<Onset> candidates;const double threshold=density<=0?1.:density==1?.55:.3;
    const double gap=60/doc.bpm*(density<=0?.8:density==1?.42:.2);
    for(const auto& o:a.onsets)if(o.time>=0&&o.time<duration&&o.strength>=threshold)candidates.push_back(o);
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& x,const auto& y){return x.strength>y.strength;});
    // Keep stronger real attacks within the density window, never insert artificial beats.
    std::map<double,Onset> kept;
    for(auto o:candidates){const auto next=kept.lower_bound(o.time);if(next!=kept.end()&&next->first-o.time<gap)continue;
        if(next!=kept.begin()&&o.time-std::prev(next)->first<gap)continue;kept.emplace(o.time,o);if(kept.size()==100000)break;}
    std::vector<chart::Note> out;std::array<int,2> alternate{};
    for(const auto& [time,o]:kept){double t=time;if(nearbySnap){const double snapped=doc.snap(t,density<=0?1:density==1?2:4);
            if(std::abs(snapped-t)<=.025&&snapped<duration)t=snapped;}
        const int registerIndex=o.lowFrequency>=.5f?0:1,lane=registerIndex*2+(alternate[registerIndex]++%2);
        out.push_back({lane,t,t});}
    return out;
}
}
