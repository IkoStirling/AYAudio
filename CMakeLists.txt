cmake_minimum_required(VERSION 3.12)
project(AYAudio
    VERSION 1.0.0
    DESCRIPTION "AYAudio �?streaming PCM ring buffer + engine-wide timeScale + resource bridge"
)

set(MODULE_NAME AYAudio)

# Core engine sources
set(SRC_FILES
    src/AYAudioBackendFactory.cpp
    src/AYAudioClipRegistry.cpp
    src/AYAudioCommandQueue.cpp
    src/AYAudioComponents.cpp
    src/AYAudioEngine.cpp
    src/AYAudioEmitterSystem.cpp
    src/AYAudioMixer.cpp
    src/AYAudioSpatializer.cpp
    src/AYAudioStreamRingBuffer.cpp
    src/AYAudioSubSystem.cpp
    src/AYAudioRuntimeModule.cpp
)

# Backends. Null + Mock are CPU-only; MiniaudioBackend talks to the
# default OS audio device via miniaudio (header-only vcpkg port).
set(BACKEND_FILES
    backend/NullBackend.cpp
    backend/MockBackend.cpp
    backend/MiniaudioBackend.cpp
)

# Backend-internal headers (kept under backend/ so they don't leak out of
# the module's public include path).
set(BACKEND_HEADERS
    backend/MiniaudioBackend.h
)

# Public headers (one-level above src/)
set(HEAD_FILES
    AYAudio.h
    include/AYAudio/AudioTypes.h
    include/AYAudio/AudioBackendFactory.h
    include/AYAudio/AudioBackendTestAccess.h
    include/AYAudio/AudioClipRegistry.h
    include/AYAudio/AudioCommandQueue.h
    include/AYAudio/AudioComponents.h
    include/AYAudio/AudioEngine.h
    include/AYAudio/AudioEmitterSystem.h
    include/AYAudio/AudioMixer.h
    include/AYAudio/AudioSpatializer.h
    include/AYAudio/AudioStreamRingBuffer.h
    include/AYAudio/AudioSubSystem.h
    include/AYAudio/AudioRuntimeModule.h
    include/AYAudio/components/AudioEmitterComponent.h
    include/AYAudio/components/AudioListenerComponent.h
    interface/AYAudio/IAudioBackend.h
)

# Locate miniaudio.h. Prefer system paths (vcpkg install layout, etc.) via
# find_path; if nothing is found, fall back to the in-tree vcpkg layout we use
# in this workspace so the build keeps working on machines without vcpkg on
# PATH. miniaudio is a header-only C library �?no link target is needed, but
# the TU that #includes it must #define MINIAUDIO_IMPLEMENTATION exactly once.
find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h")
if(NOT MINIAUDIO_INCLUDE_DIRS)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_ay_vcpkg_inc "$ENV{VCPKG_ROOT}/installed/x64-windows/include")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows/include")
        set(_ay_vcpkg_inc "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows/include")
    endif()
    if(_ay_vcpkg_inc AND EXISTS "${_ay_vcpkg_inc}/miniaudio.h")
        set(MINIAUDIO_INCLUDE_DIRS "${_ay_vcpkg_inc}")
        message(STATUS "AYAudio: using bundled vcpkg miniaudio at ${MINIAUDIO_INCLUDE_DIRS}")
    else()
        message(FATAL_ERROR
            "AYAudio: miniaudio.h not found. Install via vcpkg "
            "(`vcpkg install miniaudio:x64-windows`) or set VCPKG_ROOT.")
    endif()
endif()

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${BACKEND_FILES} ${BACKEND_HEADERS} ${HEAD_FILES})

target_include_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
    ${MINIAUDIO_INCLUDE_DIRS}
)

target_link_libraries(${MODULE_NAME} PUBLIC
    AY::Module
    AYCore
    AYMath
    AYGameLoop
    # AYResource is PUBLIC so consumers can call the new
    # AudioSubSystem::registerClipFromResource(IAudio*) bridge without
    # re-listing AYResource themselves. AYResource's own PRIVATE deps
    # (assimp/squish/basisu/AYLog) do not propagate through static lib
    # link lines on MSVC, so this doesn't drag them into AYEditor demos.
    AYResource
    # ECS AudioEmitter/Listener (v2.2). AYEntity does not link AYAudio,
    # so this edge is one-way.
    AYEntityCore
)

# Windows native deps for miniaudio (WASAPI / WinMM). Link system libs
# unconditionally so the produced AYAudio.lib is self-contained on Windows.
if(WIN32)
    target_link_libraries(${MODULE_NAME} PRIVATE winmm ole32 user32)
endif()

# C++ standard
target_compile_features(${MODULE_NAME} PRIVATE cxx_std_20)

target_compile_definitions(${MODULE_NAME} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

enable_testing()
add_subdirectory(unittest)
add_subdirectory(demo)

message(STATUS "AYAudio v1.0.0 (PCM ring buffer + engine-wide timeScale + resource bridge) �?see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
