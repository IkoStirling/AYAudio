#include <AYAudio/AudioRecorder.h>
#include <AYAudio/AudioStreamRingBuffer.h>
#include <miniaudio.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace ayt::audio {
namespace {
bool fail(std::string* e, const char* message) { if(e) *e=message; return false; }
void success(std::string* e) { if(e) e->clear(); }
bool valid(const RecordingConfig& c) {
    return c.sampleRate>=8000 && c.sampleRate<=192000 && c.channels>=1 && c.channels<=2 &&
        std::isfinite(c.maxSeconds) && c.maxSeconds*c.sampleRate>=1 && c.maxSeconds<=600 && !(c.loopback && c.monitor);
}
}
struct AudioRecorder::Impl {
    ma_context context{};
    ma_device capture{}, output{};
    bool contextReady=false, captureReady=false, outputReady=false, recording=false, limitReached=false;
    struct Endpoint { RecordingDevice info; ma_device_id native; };
    std::vector<Endpoint> endpoints;
    RecordingConfig config;
    AudioStreamRingBuffer incoming, monitoring;
    std::vector<float> pcm, peakBlocks;
    std::atomic<bool> paused{false}, monitor{false}, playing{false};
    std::atomic<uint64_t> dropped{0}, playbackFrame{0};
    std::atomic<float> inputPeak{0}, outputPeak{0};
    uint64_t maxFrames=0;
    bool initialize(std::string* error) {
        if(contextReady) return true;
#if defined(_WIN32)
        const ma_backend backend=ma_backend_wasapi;
        const auto result=ma_context_init(&backend,1,nullptr,&context);
#else
        const auto result=ma_context_init(nullptr,0,nullptr,&context);
#endif
        if(result!=MA_SUCCESS) return fail(error,"Cannot initialize recording device backend");
        contextReady=true; return true;
    }
    const ma_device_id* device(const std::string& id, bool input, bool& found) const {
        found=id.empty(); if(id.empty()) return nullptr;
        for(const auto& e:endpoints) if(e.info.id==id && e.info.input==input) {found=true;return &e.native;}
        return nullptr;
    }
    bool prepare(const RecordingConfig& c) {
        if(!valid(c)) return false;
        config=c; maxFrames=static_cast<uint64_t>(c.maxSeconds*c.sampleRate);
        if(maxFrames==0) return false;
        incoming.shutdown(); monitoring.shutdown();
        incoming.initialize(c.channels,c.sampleRate*2);
        monitoring.initialize(c.channels,c.sampleRate/2);
        pcm.clear(); peakBlocks.clear(); pcm.reserve(static_cast<size_t>(maxFrames)*c.channels);
        paused=false; dropped=0; inputPeak=0; outputPeak=0; playbackFrame=0; monitor=c.monitor; playing=false; limitReached=false;
        return true;
    }
    void receive(const float* input,uint32_t frames) {
        if(!input || paused.load(std::memory_order_relaxed)) { inputPeak=0;return; }
        float peak=0;
        for(size_t i=0;i<static_cast<size_t>(frames)*config.channels;++i)
            if(std::isfinite(input[i])) peak=std::max(peak,std::abs(input[i]));
        inputPeak.store(peak,std::memory_order_relaxed);
        const auto written=incoming.write(input,frames);
        dropped.fetch_add(frames-written,std::memory_order_relaxed);
        if(monitor.load(std::memory_order_relaxed)) monitoring.write(input,frames);
    }
    static void captureCallback(ma_device* device,void*,const void* input,ma_uint32 frames) {
        static_cast<Impl*>(device->pUserData)->receive(static_cast<const float*>(input),frames);
    }
    static void outputCallback(ma_device* device,void* target,const void*,ma_uint32 frames) {
        auto& p=*static_cast<Impl*>(device->pUserData);
        auto* out=static_cast<float*>(target);
        if(!out) return;
        std::fill_n(out,static_cast<size_t>(frames)*p.config.channels,0.f);
        if(p.monitor.load(std::memory_order_relaxed)) { p.monitoring.read(out,frames);return; }
        if(!p.playing.load(std::memory_order_relaxed)) return;
        const uint64_t total=p.pcm.size()/p.config.channels, cursor=p.playbackFrame.load(std::memory_order_relaxed);
        const auto count=static_cast<uint32_t>(std::min<uint64_t>(frames,total-std::min(cursor,total)));
        if(count) std::memcpy(out,p.pcm.data()+cursor*p.config.channels,static_cast<size_t>(count)*p.config.channels*sizeof(float));
        float peak=0;for(size_t i=0;i<static_cast<size_t>(count)*p.config.channels;++i) peak=std::max(peak,std::abs(out[i]));
        p.outputPeak.store(peak,std::memory_order_relaxed);
        p.playbackFrame.store(cursor+count,std::memory_order_relaxed);
        if(cursor+count>=total) p.playing=false;
    }
    bool openOutput(const std::string& id,std::string* error) {
        bool found=false; const auto* selected=device(id,false,found);
        if(!found) return fail(error,"Selected output device disappeared; refresh devices");
        auto c=ma_device_config_init(ma_device_type_playback);
        c.playback.pDeviceID=selected;c.playback.format=ma_format_f32;c.playback.channels=config.channels;
        c.sampleRate=config.sampleRate;c.dataCallback=outputCallback;c.pUserData=this;
        if(ma_device_init(&context,&c,&output)!=MA_SUCCESS) return fail(error,"Cannot open selected output device");
        outputReady=true;
        if(ma_device_start(&output)!=MA_SUCCESS) { closeOutput();return fail(error,"Cannot start output device"); }
        return true;
    }
    void closeOutput() {
        if(outputReady) {ma_device_uninit(&output);outputReady=false;}
        playing=false;monitor=false;outputPeak=0;
    }
    void drain() {
        float chunk[4096]; const uint32_t blockFrames=4096/config.channels;
        uint32_t count=0;
        while((count=incoming.read(chunk,blockFrames))!=0) {
            uint64_t before=pcm.size()/config.channels;
            const auto accepted=static_cast<uint32_t>(std::min<uint64_t>(count,maxFrames-std::min(before,maxFrames)));
            for(uint32_t f=0;f<accepted;++f) {
                const size_t bin=static_cast<size_t>((before+f)/128);
                if(peakBlocks.size()<=bin) peakBlocks.push_back(0);
                for(uint16_t ch=0;ch<config.channels;++ch) {
                    const float raw=chunk[f*config.channels+ch];
                    const float sample=std::isfinite(raw)?std::clamp(raw,-1.f,1.f):0.f;
                    pcm.push_back(sample);peakBlocks[bin]=std::max(peakBlocks[bin],std::abs(sample));
                }
            }
            if(before+accepted>=maxFrames) limitReached=true;
        }
    }
};
AudioRecorder::AudioRecorder():_impl(std::make_unique<Impl>()) {}
AudioRecorder::~AudioRecorder() {stop();stopPlayback();if(_impl->contextReady) ma_context_uninit(&_impl->context);}
bool AudioRecorder::enumerateDevices(std::vector<RecordingDevice>& out,std::string* error) {
    out.clear();auto& p=*_impl;
    if(p.recording || p.outputReady) return fail(error,"Stop recording/playback before refreshing devices");
    if(!p.initialize(error)) return false;
    ma_device_info *playback=nullptr,*capture=nullptr;ma_uint32 nplay=0,ncap=0;
    if(ma_context_get_devices(&p.context,&playback,&nplay,&capture,&ncap)!=MA_SUCCESS) return fail(error,"Cannot enumerate audio devices");
    p.endpoints.clear();
    for(int input=0;input<2;++input) {
        const auto count=input?ncap:nplay; const auto* infos=input?capture:playback;
        for(ma_uint32 i=0;i<count;++i) {
            RecordingDevice d{(input?"input/":"output/")+std::to_string(i),infos[i].name,input!=0,infos[i].isDefault!=0};
            p.endpoints.push_back({d,infos[i].id});out.push_back(d);
        }
    }
    success(error);return true;
}
bool AudioRecorder::start(const RecordingConfig& c,std::string* error) {
    auto& p=*_impl;
    if(p.recording) return fail(error,"Recording is already active");
    if(!valid(c)) return fail(error,"Invalid recording settings (maximum duration: 600 seconds)");
    stopPlayback();if(!p.initialize(error)) return false;
    bool found=false;const auto* selected=p.device(c.loopback?c.outputDevice:c.inputDevice,!c.loopback,found);
    if(!found) return fail(error,"Selected input device disappeared; refresh devices");
#if !defined(_WIN32)
    if(c.loopback) return fail(error,"Output loopback recording requires WASAPI on Windows");
#endif
    auto native=ma_device_config_init(c.loopback?ma_device_type_loopback:ma_device_type_capture);
    native.capture.pDeviceID=selected;native.capture.format=ma_format_f32;native.capture.channels=c.channels;
    native.sampleRate=c.sampleRate;native.dataCallback=Impl::captureCallback;native.pUserData=&p;
    if(ma_device_init(&p.context,&native,&p.capture)!=MA_SUCCESS) return fail(error,"Cannot open selected capture device (check device/permissions)");
    p.captureReady=true;
    // Preserve the old take on open/start failure. No callback mutates pcm directly.
    auto oldPcm=std::move(p.pcm);auto oldPeaks=std::move(p.peakBlocks);const auto oldConfig=p.config;
    try {p.prepare(c);}catch(const std::exception&) {
        ma_device_uninit(&p.capture);p.captureReady=false;
        p.pcm=std::move(oldPcm);p.peakBlocks=std::move(oldPeaks);p.config=oldConfig;
        return fail(error,"Cannot allocate recording buffer; previous take retained");
    }
    bool ok=!c.monitor || p.openOutput(c.outputDevice,error);
    if(ok) ok=ma_device_start(&p.capture)==MA_SUCCESS;
    if(!ok) {
        ma_device_uninit(&p.capture);p.captureReady=false;p.closeOutput();
        p.pcm=std::move(oldPcm);p.peakBlocks=std::move(oldPeaks);p.config=oldConfig;
        return fail(error,"Cannot start recording; previous take retained");
    }
    p.recording=true;success(error);return true;
}
void AudioRecorder::pause(bool paused) {if(_impl->recording) _impl->paused=paused;}
void AudioRecorder::update() {auto& p=*_impl;if(p.recording) {p.drain();if(p.limitReached) stop();}if(p.outputReady && !p.monitor && !p.playing) stopPlayback();}
void AudioRecorder::stop() {
    auto& p=*_impl;
    if(p.captureReady) {ma_device_uninit(&p.capture);p.captureReady=false;}
    if(p.recording) {p.drain();p.recording=false;}
    if(p.monitor) p.closeOutput();p.paused=false;p.inputPeak=0;
}
bool AudioRecorder::play(const std::string& output,std::string* error) {
    auto& p=*_impl;
    if(p.recording || p.pcm.empty()) return fail(error,"Stop recording and record a take before playback");
    stopPlayback();if(!p.initialize(error)) return false;p.playbackFrame=0;p.monitor=false;p.playing=true;
    if(!p.openOutput(output,error)) {p.playing=false;return false;}
    success(error);return true;
}
void AudioRecorder::stopPlayback() {_impl->closeOutput();}
bool AudioRecorder::clear() {auto& p=*_impl;if(p.recording) return false;stopPlayback();p.pcm.clear();p.peakBlocks.clear();p.limitReached=false;p.dropped=0;return true;}
RecordingStatus AudioRecorder::status() const {
    const auto& p=*_impl;return {p.recording,p.paused.load(),p.playing.load(),p.limitReached,
        p.pcm.size()/p.config.channels,p.dropped.load(),p.config.sampleRate,p.config.channels,p.recording?p.inputPeak.load():p.outputPeak.load(),
        static_cast<double>(p.playbackFrame.load())/p.config.sampleRate};
}
const std::vector<float>& AudioRecorder::samples() const {return _impl->pcm;}
std::vector<float> AudioRecorder::waveform(size_t bins) const {
    const auto& peaks=_impl->peakBlocks; if(peaks.empty() || bins==0) return {};
    bins=std::min(bins,peaks.size());std::vector<float> result(bins,0);
    for(size_t i=0;i<peaks.size();++i) result[std::min(bins-1,i*bins/peaks.size())]=std::max(result[std::min(bins-1,i*bins/peaks.size())],peaks[i]);
    return result;
}
bool AudioRecorder::saveWav(const std::string& assetRoot,const std::string& virtualPath,std::string* error) const {
    namespace fs=std::filesystem;const auto& p=*_impl;
    if(p.recording || p.pcm.empty()) return fail(error,"Stop recording before saving a non-empty take");
    if(assetRoot.empty() || virtualPath.empty()) return fail(error,"No writable project asset root/path");
    try {
        const auto ref=fs::u8path(virtualPath);
        if(ref.is_absolute() || ref.has_root_name() || ref.extension()!=L".wav") return fail(error,"Use a relative .wav virtual path");
        for(const auto& part:ref) if(part==L"..") return fail(error,"Recording path must stay inside project assets");
        const auto root=fs::weakly_canonical(fs::u8path(assetRoot));
        const auto destination=fs::weakly_canonical(root/ref);
        const auto relative=destination.lexically_relative(root);
        if(relative.empty() || relative.is_absolute() || *relative.begin()==L"..") return fail(error,"Recording path escapes project assets");
        if(fs::exists(destination)) return fail(error,"Destination already exists; choose a new recording name");
        fs::create_directories(destination.parent_path());
        const auto size=p.pcm.size()*2;
        if(size>std::numeric_limits<uint32_t>::max()-36) return fail(error,"Recording exceeds WAV size limit");
        // A unique temporary sibling is never allowed to overwrite an existing file.
        auto temp=destination;temp+=L".recording.tmp";
        if(fs::exists(temp)) return fail(error,"Temporary recording file already exists");
        bool written=false;
        try {
            std::ofstream file(temp,std::ios::binary);
            auto u16=[&](uint16_t v) {char b[2]={static_cast<char>(v),static_cast<char>(v>>8)};file.write(b,2);};
            auto u32=[&](uint32_t v) {char b[4]={static_cast<char>(v),static_cast<char>(v>>8),static_cast<char>(v>>16),static_cast<char>(v>>24)};file.write(b,4);};
            file.write("RIFF",4);u32(static_cast<uint32_t>(size+36));file.write("WAVEfmt ",8);u32(16);u16(1);u16(p.config.channels);
            u32(p.config.sampleRate);u32(p.config.sampleRate*p.config.channels*2);u16(p.config.channels*2);u16(16);
            file.write("data",4);u32(static_cast<uint32_t>(size));
            char encoded[16384];
            for(size_t offset=0;offset<p.pcm.size();offset+=8192) {
                const auto count=std::min<size_t>(8192,p.pcm.size()-offset);
                for(size_t i=0;i<count;++i) {
                    const auto value=static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(p.pcm[offset+i],-1.f,1.f)*32767)));
                    encoded[i*2]=static_cast<char>(value);encoded[i*2+1]=static_cast<char>(value>>8);
                }
                file.write(encoded,static_cast<std::streamsize>(count*2));
            }
            file.flush();written=file.good();file.close();
            if(!written) {fs::remove(temp);return fail(error,"Cannot write recording WAV");}
            if(fs::exists(destination)) {fs::remove(temp);return fail(error,"Destination appeared during save; choose a new name");}
#if defined(_WIN32)
            if(!::MoveFileW(temp.c_str(),destination.c_str()))
                throw fs::filesystem_error("Cannot publish recording",temp,destination,
                    std::error_code(static_cast<int>(::GetLastError()),std::system_category()));
#else
            fs::create_hard_link(temp,destination); // exclusive publish: never replace an existing take
            std::error_code ignored;fs::remove(temp,ignored);
#endif
        } catch(...) {std::error_code ignored;fs::remove(temp,ignored);throw;}
        success(error);return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool AudioRecorderTestAccess::beginBuffered(AudioRecorder& recorder,const RecordingConfig& c) {
    recorder.stop();recorder.stopPlayback();if(!recorder._impl->prepare(c)) return false;recorder._impl->recording=true;return true;
}
void AudioRecorderTestAccess::receive(AudioRecorder& recorder,const float* pcm,uint32_t frames) {if(recorder._impl->recording) recorder._impl->receive(pcm,frames);}
} // namespace ayt::audio
