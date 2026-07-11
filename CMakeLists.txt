cmake_minimum_required(VERSION 3.12)
project(AYAudio)

set(MODULE_NAME AYAudio)

# Core engine sources
set(SRC_FILES
    src/AYAudioBackendFactory.cpp
    src/AYAudioClipRegistry.cpp
    src/AYAudioCommandQueue.cpp
    src/AYAudioEngine.cpp
    src/AYAudioMixer.cpp
    src/AYAudioSpatializer.cpp
    src/AYAudioSubSystem.cpp
)

# Backends (Null + Mock ship in v0; MiniaudioBackend is a stub until v0.5)
set(BACKEND_FILES
    backend/NullBackend.cpp
    backend/MockBackend.cpp
    backend/MiniaudioBackend.cpp
)

# Public headers (one-level above src/)
set(HEAD_FILES
    AYAudio.h
    include/AYAudioTypes.h
    include/AYAudioBackendFactory.h
    include/AYAudioBackendTestAccess.h
    include/AYAudioClipRegistry.h
    include/AYAudioCommandQueue.h
    include/AYAudioEngine.h
    include/AYAudioMixer.h
    include/AYAudioSpatializer.h
    include/AYAudioSubSystem.h
    interface/IAudioBackend.h
)

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${BACKEND_FILES} ${HEAD_FILES})

target_include_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(${MODULE_NAME} PUBLIC
    AYCore
    AYMath
    AYGameLoop
)

# C++ standard
target_compile_features(${MODULE_NAME} PRIVATE cxx_std_20)

target_compile_definitions(${MODULE_NAME} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

enable_testing()
add_subdirectory(unittest)