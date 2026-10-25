#include <AYAudio/OutputDiagnostics.h>
#include <AYTestFixtures.h>
#include <algorithm>
#include <cmath>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#include <objbase.h>
#endif
using namespace ayt::audio;
namespace {
bool valid(const OutputVolume& value){return !value.known||(std::isfinite(value.master)&&value.master>=0&&value.master<=1&&value.channels.size()<=32&&std::all_of(value.channels.begin(),value.channels.end(),[](float v){return std::isfinite(v)&&v>=0&&v<=1;}));}
}
TEST_SUITE(OutputDiagnostics)
TEST_CASE(read_only_snapshot_is_bounded_and_never_includes_other_process_sessions){
    const auto result=queryProcessOutput();
#ifdef _WIN32
    CHECK(result.status!=OutputQueryStatus::Unsupported);
    CHECK(result.console.name.size()<=2048&&result.multimedia.name.size()<=2048);
    CHECK(valid(result.console.volume)&&valid(result.multimedia.volume));
    // This process has not created an audio stream. Other applications playing
    // on the same endpoints must not appear as this process's sessions.
    CHECK(result.processSessions.empty());
#else
    CHECK(result.status==OutputQueryStatus::Unsupported);CHECK(!result.console.available&&!result.multimedia.available);CHECK(result.processSessions.empty());
#endif
}
TEST_CASE(caller_apartment_is_preserved_and_repeated_queries_release_native_handles){
#ifdef _WIN32
    bool preserved=false;std::thread sta([&]{const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return;const auto first=queryProcessOutput();const auto again=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(SUCCEEDED(again))CoUninitialize();preserved=again==S_FALSE;CoUninitialize();});sta.join();CHECK(preserved);
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(int i=0;i<20;++i){const auto observation=queryProcessOutput();CHECK(observation.processSessions.size()<=32);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));CHECK(after<=before+16);
#else
    CHECK(queryProcessOutput().status==OutputQueryStatus::Unsupported);
#endif
}
TEST_SUITE_END
int main(int argc,char** argv){return ayt::test::runTests("AYAudioOutputDiagnostics",argc,argv);}
