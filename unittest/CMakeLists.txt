cmake_minimum_required(VERSION 3.15)

# AYAudio Unit Tests
project(AYAudio_Tests)

set(TEST_TARGET AYAudio_Tests)

add_executable(${TEST_TARGET}
    main.cpp
    Test_AudioCommandQueue.cpp
    Test_AudioClipRegistry.cpp
    Test_AudioSpatializer.cpp
    Test_AudioMixer.cpp
    Test_AudioEngine.cpp
    Test_MiniaudioBackend.cpp
    Test_AudioSubSystemBridge.cpp
)

target_link_libraries(${TEST_TARGET} PRIVATE
    AYTest
    AYAudio
    AYMath
)

target_include_directories(${TEST_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend
    # vcpkg miniaudio header (needed by tests that include MiniaudioBackend.h
    # which forward-declares ma_device).
    ${MINIAUDIO_INCLUDE_DIRS}
)

target_compile_definitions(${TEST_TARGET} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_options(${TEST_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4 /wd4200>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})