# Upstream https://github.com/hoene/libmysofa tag v1.3.5, unmodified library sources.
# Archive SHA256 f29508c335c83d8703f943ffc9ca783ac39aca84e851357f13a55af0f8143137.
# BSD-3-Clause (LICENSE); embedded Speex resampler retains its own BSD notice.
# This wrapper uses the engine's ZLIB, avoiding upstream's MSVC NuGet invocation.
enable_language(C)
find_package(ZLIB REQUIRED)
set(_sources hrtf/reader.c hdf/superblock.c hdf/dataobject.c hdf/btree.c
 hdf/fractalhead.c hdf/gunzip.c hdf/gcol.c hrtf/check.c hrtf/spherical.c
 hrtf/lookup.c hrtf/tools.c hrtf/kdtree.c hrtf/neighbors.c hrtf/interpolate.c
 hrtf/resample.c hrtf/loudness.c hrtf/minphase.c hrtf/easy.c hrtf/cache.c
 resampler/speex_resampler.c)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mysofa_export.h" CONTENT "#pragma once\n#define MYSOFA_EXPORT\n")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/config.h" CONTENT "#pragma once\n#define CMAKE_INSTALL_PREFIX \"\"\n#define CPACK_PACKAGE_VERSION_MAJOR 1\n#define CPACK_PACKAGE_VERSION_MINOR 3\n#define CPACK_PACKAGE_VERSION_PATCH 5\n")
add_library(AYAudio_MySofa STATIC ${_sources})
target_include_directories(AYAudio_MySofa PUBLIC hrtf "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(AYAudio_MySofa PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
target_link_libraries(AYAudio_MySofa PRIVATE ZLIB::ZLIB)
if(UNIX)
 target_link_libraries(AYAudio_MySofa PRIVATE m)
endif()
