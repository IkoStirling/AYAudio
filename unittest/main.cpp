// AYAudio unit test entry point.
//
// Each Test_*.cpp is its own translation unit (listed in CMakeLists.txt).
// Static initializers in those TUs register TEST_SUITE / TEST_CASE entries;
// main() only runs the AYTest runner.
//
// Do NOT #include Test_*.cpp here — that duplicates registrations and can
// pull Windows.h ahead of AYMath in some MSVC include orders.

#include "AYTest.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    return ayt::test::runAllTests("AYAudio");
}