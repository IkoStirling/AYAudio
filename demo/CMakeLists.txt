# AYAudio/demo — standalone mixer gallery + AYUI Audio Editor.

# ---------------------------------------------------------------------------
# AYAudio_Gallery — Win32 smoke (no AYUI)
# ---------------------------------------------------------------------------
add_executable(AYAudio_Gallery AYAudioGallery.cpp)

target_link_libraries(AYAudio_Gallery PRIVATE AYAudio)

if(WIN32)
    target_link_libraries(AYAudio_Gallery PRIVATE user32 gdi32 comctl32 comdlg32)
endif()

if(MSVC)
    target_compile_options(AYAudio_Gallery PRIVATE /Zc:__cplusplus /Zc:preprocessor)
    target_compile_definitions(AYAudio_Gallery PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

# ---------------------------------------------------------------------------
# AYAudio_AudioEditor — AYUI mixer desk (shared session with AYEditor)
# ---------------------------------------------------------------------------
set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../AYShader")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")

add_executable(AYAudio_AudioEditor WIN32
    audio_editor/AYAudio_AudioEditor.cpp
    audio_editor/AudioEditorSession.cpp
    audio_editor/AudioEditorSession.h
)

target_include_directories(AYAudio_AudioEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_editor
)

target_link_libraries(AYAudio_AudioEditor PRIVATE
    AYAudio
    AYUI
    AYRenderer
    AYDevice
)

if(WIN32)
    target_link_libraries(AYAudio_AudioEditor PRIVATE user32 gdi32 comdlg32)
endif()

if(MSVC)
    target_compile_options(AYAudio_AudioEditor PRIVATE /Zc:__cplusplus /Zc:preprocessor)
    target_compile_definitions(AYAudio_AudioEditor PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

target_compile_definitions(AYAudio_AudioEditor PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
)

add_custom_command(TARGET AYAudio_AudioEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/audio_editor/assets
        $<TARGET_FILE_DIR:AYAudio_AudioEditor>/assets
    COMMENT "Copy Audio Editor assets next to AYAudio_AudioEditor"
)
