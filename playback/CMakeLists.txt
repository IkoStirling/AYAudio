cmake_minimum_required(VERSION 3.20)
if(TARGET AYAudioPlayback)
    return()
endif()
get_filename_component(_audio "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT TARGET AYAudioPan)
    add_subdirectory("${_audio}/pan" "${CMAKE_CURRENT_BINARY_DIR}/pan")
endif()
if(NOT TARGET AYAudio_MySofa)
    add_subdirectory("${_audio}/thirdparty/libmysofa" "${CMAKE_CURRENT_BINARY_DIR}/mysofa")
endif()
find_path(MINIAUDIO_INCLUDE_DIRS miniaudio.h REQUIRED)
add_library(AYAudioPlayback STATIC
    "${_audio}/src/AYAudioBackendFactory.cpp"
    "${_audio}/src/AYAudioClipRegistry.cpp"
    "${_audio}/src/AYAudioCommandQueue.cpp"
    "${_audio}/src/AYAudioEngine.cpp"
    "${_audio}/src/AYAudioMixer.cpp"
    "${_audio}/src/AYAudioProcessing.cpp"
    "${_audio}/src/AYAudioSpatializer.cpp"
    "${_audio}/src/AYAudioStreamRingBuffer.cpp"
    "${_audio}/backend/NullBackend.cpp"
    "${_audio}/backend/MockBackend.cpp"
    "${_audio}/backend/MiniaudioBackend.cpp")
target_include_directories(AYAudioPlayback PUBLIC "${_audio}" "${_audio}/include" "${_audio}/interface"
    PRIVATE "${_audio}/src" "${_audio}/backend" "${MINIAUDIO_INCLUDE_DIRS}")
target_link_libraries(AYAudioPlayback PUBLIC AYCore AYMath AYAudioPan PRIVATE AYAudio_MySofa)
target_compile_features(AYAudioPlayback PUBLIC cxx_std_20)
target_compile_definitions(AYAudioPlayback PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_link_libraries(AYAudioPlayback PRIVATE winmm ole32 user32)
endif()
if(BUILD_TESTING AND TARGET AYTest)
    add_executable(AYAudioPlayback_Tests Test_Playback.cpp)
    target_link_libraries(AYAudioPlayback_Tests PRIVATE AYAudioPlayback AYTest)
    ay_add_test(AYAudioPlayback_Tests)
endif()
