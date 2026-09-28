// AYAudio unit test entry point.
//
// Each Test_*.cpp is its own translation unit (listed in CMakeLists.txt).
// Static initializers in those TUs register TEST_SUITE / TEST_CASE entries;
// main() only runs the AYTest runner.
//
// Do NOT #include Test_*.cpp here — that duplicates registrations and can
// pull Windows.h ahead of AYMath in some MSVC include orders.

#include "AYTest.h"

#include <AYAudio/AudioComponents.h>
#include <AYEntity/EntityModule.h>

int main(int argc, char* argv[]) {
    ayt::test::RunOptions options;
    bool help = false;
    if (ayt::test::parseRunOptions(argc, argv, options, help)) return 2;
    if (help || options.list) return ayt::test::runTests("AYAudio", argc, argv);
    ayt::entity::registerEntityComponents();
    ayt::audio::registerAudioComponents();
    return ayt::test::runTests("AYAudio", options);
}
