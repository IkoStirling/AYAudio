#include <AYAudio/AudioEngine.h>
#include <AYTestFixtures.h>
#include <algorithm>
#include <vector>
using namespace ayt::audio;
TEST_SUITE(StandaloneAudioPlayback)
TEST_CASE(stream_pcm_changes_direction_without_restarting_voice){
    AudioEngine engine;AudioSettings settings;settings.sampleRate=24000;settings.reverbReturn=0;CHECK(engine.initialize(settings));
    AudioStreamDesc desc;desc.channels=1;desc.sampleRate=24000;desc.capacityFrames=4096;
    const auto stream=engine.openStream(desc);CHECK(stream!=0);std::vector<float> pcm(4096,.25f);CHECK(engine.streamPush(stream,pcm.data(),4096)==4096);
    SpatializationParams spatial;spatial.stereoPan=-1;const auto voice=engine.playStream(stream,AudioBus::Voice,spatial);CHECK(voice!=0);
    std::vector<float> output(1024);engine.render(output.data(),512);
    double left=0,right=0;for(size_t i=0;i<output.size();i+=2){left+=std::abs(output[i]);right+=std::abs(output[i+1]);}CHECK(left>10);CHECK(right<.001);
    const auto before=engine.voicePositionFrames(voice);spatial.stereoPan=1;engine.setVoiceSpatial(voice,spatial);engine.submitFrame(.02f);engine.render(output.data(),512);
    left=right=0;for(size_t i=0;i<output.size();i+=2){left+=std::abs(output[i]);right+=std::abs(output[i+1]);}CHECK(right>left);CHECK(engine.voicePositionFrames(voice)>before);
    engine.stop(voice);engine.closeStream(stream);engine.shutdown();CHECK(!engine.isInitialized());
}
TEST_CASE(clips_resample_and_shutdown_is_repeatable){
    AudioEngine engine;AudioSettings settings;settings.sampleRate=24000;settings.reverbReturn=0;
    for(int repeat=0;repeat<3;++repeat){CHECK(engine.initialize(settings));std::vector<float> pcm(4800,.1f);AudioClipDesc desc;desc.channels=1;desc.sampleRate=48000;desc.frameCount=4800;desc.pcmData=pcm.data();const auto clip=engine.registerClip(desc);CHECK(clip!=0);const auto voice=engine.play(clip);CHECK(voice!=0);std::vector<float> output(512);engine.render(output.data(),256);CHECK(engine.voicePositionFrames(voice)==256);engine.stop(voice);engine.releaseClip(clip);engine.shutdown();}
}
TEST_CASE(speech_cursor_waits_for_source_during_underflow){
    AudioEngine engine;AudioSettings settings;settings.sampleRate=24000;settings.reverbReturn=0;CHECK(engine.initialize(settings));
    AudioStreamDesc desc;desc.channels=1;desc.sampleRate=24000;desc.advanceClockOnUnderrun=false;const auto stream=engine.openStream(desc);
    float pcm[4]={.1f,.2f,.3f,.4f};CHECK(engine.streamPush(stream,pcm,4)==4);const auto voice=engine.playStream(stream);float out[32]{};engine.render(out,16);CHECK(engine.voicePositionFrames(voice)==4);
    engine.render(out,16);CHECK(engine.voicePositionFrames(voice)==4);CHECK(std::all_of(std::begin(out),std::end(out),[](float x){return x==0;}));
    CHECK(engine.streamPush(stream,pcm,4)==4);engine.render(out,16);CHECK(engine.voicePositionFrames(voice)==8);engine.stop(voice);engine.closeStream(stream);
}
TEST_SUITE_END
int main(int argc,char** argv){return ayt::test::runTests("AYAudioPlayback",argc,argv);}
