find_package(nlohmann_json CONFIG REQUIRED)
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
    Test_AudioProcessing.cpp
    Test_AudioContentPlayer.cpp
    Test_MidiInput.cpp
    Test_AudioLatencyDevice.cpp
    Test_AudioEngine.cpp
    Test_AudioRecorder.cpp
    Test_ChartDocument.cpp
    Test_Performance.cpp
    Test_SampleInstrument.cpp
    Test_MiniaudioBackend.cpp
    Test_AudioSubSystemBridge.cpp
    Test_AudioStreamRingBuffer.cpp
    Test_AudioEngineStream.cpp
    Test_AudioSampleRateAndHrtf.cpp
    Test_AudioStreamingBgm.cpp
    Test_AudioEmitterSystem.cpp
    Test_AudioRuntimeModule.cpp
)

target_link_libraries(${TEST_TARGET} PRIVATE
    AYTest
    nlohmann_json::nlohmann_json
    AYAudio
    AYMath
    AYEntity
    AYIO
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

ay_add_test_tiers(${TEST_TARGET} PREFIX audio DEFAULT_TIER integration
    FAST_SUITES AudioCommandQueueTests AudioClipRegistryTests
                AudioSpatializerTests AudioHrtfTests AudioStreamRingBufferTests
    TIMEOUT 120 RUN_SERIAL)

if(TARGET AYUI AND TARGET AYUICurveEditor)
    add_executable(AYAudio_AuthoringTests authoring_main.cpp Test_AudioAuthoring.cpp ../demo/audio_editor/AudioEditorSession.cpp)
    target_link_libraries(AYAudio_AuthoringTests PRIVATE AYAudio AYTest AYUI AYUICurveEditor AYProject nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(AYAudio_AuthoringTests PRIVATE user32 ole32 shell32)
    endif()
    target_compile_definitions(AYAudio_AuthoringTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(AYAudio_AuthoringTests PRIVATE /bigobj)
    endif()
    ay_add_test_tiers(AYAudio_AuthoringTests PREFIX audio-authoring DEFAULT_TIER integration TIMEOUT 120 RUN_SERIAL)
endif()
