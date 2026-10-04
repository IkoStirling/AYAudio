#include "AYTest.h"
#include <AYAudio/AudioRecorder.h>
#include <AYResource/Loader/AudioLoader.h>
#include <AYResource/assetsImpl/Audio.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdio>
using namespace ayt::audio;
TEST_SUITE(AudioRecorderTests)
TEST_CASE(BufferedCapturePauseStopLimitsAndWaveform) {
    AudioRecorder r;RecordingConfig c;c.sampleRate=8000;c.channels=2;c.maxSeconds=.01;
    CHECK(AudioRecorderTestAccess::beginBuffered(r,c));
    float pcm[200];for(int i=0;i<200;++i) pcm[i]=i%2?.5f:-.25f;
    AudioRecorderTestAccess::receive(r,pcm,20);r.update();
    CHECK(r.status().frames==20);CHECK(r.status().recording);
    r.pause(true);AudioRecorderTestAccess::receive(r,pcm,20);r.update();CHECK(r.status().frames==20);
    r.pause(false);AudioRecorderTestAccess::receive(r,pcm,100);r.update();
    CHECK(r.status().frames==80);CHECK_FALSE(r.status().recording);CHECK(r.status().limitReached);
    CHECK(r.samples().size()==160);CHECK(r.samples()[0]==-.25f);CHECK(r.samples()[1]==.5f);
    const auto wave=r.waveform();CHECK(wave.size()==1);CHECK(wave[0]==.5f);
    r.stop();CHECK(r.status().frames==80);CHECK(r.clear());CHECK(r.samples().empty());
    c.maxSeconds=0;CHECK_FALSE(AudioRecorderTestAccess::beginBuffered(r,c));
    c.maxSeconds=1;c.channels=3;CHECK_FALSE(AudioRecorderTestAccess::beginBuffered(r,c));
    c.channels=2;c.loopback=c.monitor=true;CHECK_FALSE(AudioRecorderTestAccess::beginBuffered(r,c));
}
TEST_CASE(CaptureOverflowAndNonFiniteSamplesAreExplicit) {
    AudioRecorder r;RecordingConfig c;c.sampleRate=8000;c.channels=1;c.maxSeconds=5;
    CHECK(AudioRecorderTestAccess::beginBuffered(r,c));
    std::vector<float> samples(24000,2);samples[0]=std::nanf("");samples[1]=-3;
    AudioRecorderTestAccess::receive(r,samples.data(),24000);CHECK(r.status().droppedFrames>0);
    CHECK_FALSE(r.clear());r.stop();CHECK(r.status().frames==16384);
    CHECK(r.samples()[0]==0);CHECK(r.samples()[1]==-1);CHECK(r.samples()[2]==1);
    CHECK(r.waveform(8).size()==8);CHECK(r.waveform(0).empty());
}
TEST_CASE(CallbackAndOwnerDrainPreserveOrderedPcm) {
    AudioRecorder r;RecordingConfig c;c.sampleRate=8000;c.channels=1;c.maxSeconds=2;
    CHECK(AudioRecorderTestAccess::beginBuffered(r,c));
    std::atomic<bool> finished=false;
    std::thread producer([&] {float block[32];for(int n=0;n<100;++n) {
        std::fill_n(block,32,n/100.f);AudioRecorderTestAccess::receive(r,block,32);
    }finished=true;});
    while(!finished) {r.update();std::this_thread::yield();}producer.join();r.stop();
    CHECK(r.status().frames==3200);CHECK(r.status().droppedFrames==0);
    for(int n=0;n<100;++n) CHECK(std::abs(r.samples()[n*32]-n/100.f)<.00001f);
}
TEST_CASE(ProjectVirtualWavSaveRoundtripAndBoundary) {
    namespace fs=std::filesystem;
    const auto root=ayt::test::testTmpDir()/"recording_assets";
    struct Cleanup {fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{root};
    fs::create_directories(root);
    AudioRecorder r;RecordingConfig c;c.sampleRate=8000;c.channels=2;c.maxSeconds=1;
    CHECK(AudioRecorderTestAccess::beginBuffered(r,c));
    std::vector<float> pcm(1600);for(size_t i=0;i<pcm.size();++i) pcm[i]=static_cast<float>(std::sin(i*.06))*.5f;
    AudioRecorderTestAccess::receive(r,pcm.data(),800);
    std::string error;CHECK_FALSE(r.saveWav(root.string(),"audio/recordings/take.wav",&error));
    CHECK_FALSE(error.empty());r.stop();
    CHECK_FALSE(r.saveWav("","audio/take.wav",&error));
    CHECK_FALSE(r.saveWav(root.string(),"../escape.wav",&error));
    CHECK_FALSE(r.saveWav(root.string(),(root/"absolute.wav").string(),&error));
    CHECK_FALSE(r.saveWav(root.string(),"audio/take.mp3",&error));
    CHECK(r.saveWav(root.string(),"audio/recordings/take.wav",&error));CHECK(error.empty());
    const auto path=root/"audio/recordings/take.wav";CHECK(fs::file_size(path)==44+3200);
    ayt::resource::AudioLoader loader;
    const auto audio=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(path.string()));
    CHECK(audio!=nullptr);CHECK(audio->getChannels()==2);CHECK(audio->getSampleRate()==8000);CHECK(audio->getDataSize()==3200);
    CHECK_FALSE(r.saveWav(root.string(),"audio/recordings/take.wav",&error));CHECK(fs::file_size(path)==3244);
    c.sampleRate=3;CHECK_FALSE(r.start(c,&error));CHECK(r.status().frames==800);
    const auto outside=root.parent_path()/"recording_outside";fs::create_directories(outside);Cleanup outsideCleanup{outside};
    std::error_code ec;fs::create_directory_symlink(outside,root/"linked",ec);
    if(!ec) CHECK_FALSE(r.saveWav(root.string(),"linked/escaped.wav",&error));
    CHECK_FALSE(fs::exists(outside/"escaped.wav"));
}
TEST_SUITE_END

TEST_SUITE(AudioRecorderDeviceTests)
TEST_CASE(WasapiLoopbackCapturesGeneratedTakeAndSaves) {
#if !defined(_WIN32)
    SKIP_TEST("WASAPI recording smoke is Windows-only");
#else
    if(!std::getenv("AY_AUDIO_RECORDING_DEVICE_TEST")) SKIP_TEST("Opt-in device test: AY_AUDIO_RECORDING_DEVICE_TEST=1");
    AudioRecorder capture,emitter;std::vector<RecordingDevice> devices;std::string error;
    CHECK(capture.enumerateDevices(devices,&error));
    for(const auto& d:devices) std::fprintf(stderr,"[RecordingDevice] %s %s %s\n",d.input?"input":"output",d.id.c_str(),(d.name+(d.isDefault?" [default]":"")).c_str());
    RecordingConfig config;config.maxSeconds=2;config.loopback=true;
    for(const auto& d:devices) if(!d.input && d.isDefault) config.outputDevice=d.id;
    const auto selected=std::find_if(devices.begin(),devices.end(),[&](const auto& d){return d.id==config.outputDevice && !d.input;});
    CHECK(selected!=devices.end());
    std::vector<RecordingDevice> outputDevices;CHECK(emitter.enumerateDevices(outputDevices,&error));
    std::string playbackId;
    if(selected!=devices.end()) for(const auto& d:outputDevices) if(!d.input && d.name==selected->name) playbackId=d.id;
    CHECK(!playbackId.empty());
    CHECK(capture.start(config,&error));
    config.loopback=false;CHECK(AudioRecorderTestAccess::beginBuffered(emitter,config));
    std::vector<float> tone(48000);
    for(size_t f=0;f<tone.size()/2;++f) tone[f*2]=tone[f*2+1]=static_cast<float>(std::sin(f*6.28318530718*440/48000))*.1f;
    AudioRecorderTestAccess::receive(emitter,tone.data(),24000);emitter.stop();CHECK(emitter.status().frames==24000);CHECK(emitter.play(playbackId,&error));
    for(int i=0;i<90;++i) {std::this_thread::sleep_for(std::chrono::milliseconds(10));capture.update();emitter.update();}
    capture.stop();emitter.stopPlayback();const auto status=capture.status();
    std::fprintf(stderr,"[RecordingSmoke] frames=%llu dropped=%llu\n",static_cast<unsigned long long>(status.frames),static_cast<unsigned long long>(status.droppedFrames));
    CHECK(status.frames>24000);CHECK(status.droppedFrames==0);
    const auto peaks=capture.waveform();CHECK(!peaks.empty());
    std::fprintf(stderr,"[RecordingSmoke] peak=%.6f playback_frame=%.3f endpoint=%s\n",*std::max_element(peaks.begin(),peaks.end()),emitter.status().positionSeconds,selected!=devices.end()?selected->name.c_str():"default");
    CHECK(*std::max_element(peaks.begin(),peaks.end())>.001f);
    const auto root=ayt::test::testTmpDir()/"native_recording_assets";
    CHECK(capture.saveWav(root.string(),"audio/recordings/device_smoke.wav",&error));
    ayt::resource::AudioLoader loader;CHECK(loader.load((root/"audio/recordings/device_smoke.wav").string())!=nullptr);
    std::error_code ec;std::filesystem::remove_all(root,ec);
#endif
}
TEST_SUITE_END
