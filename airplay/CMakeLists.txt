# SPDX-License-Identifier: GPL-3.0-or-later
get_filename_component(_cloudtune_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_package(Threads REQUIRED)
if(PLATFORM_SWITCH OR NINTENDO_SWITCH)
  set(_codec_platform switch)
else()
  set(_codec_platform host)
endif()
set(CLOUDTUNE_FFMPEG_ROOT "${_cloudtune_root}/.deps/ffmpeg-${_codec_platform}"
  CACHE PATH "Pinned ALAC-only FFmpeg installation")
if(NOT EXISTS "${CLOUDTUNE_FFMPEG_ROOT}/lib/libavcodec.a")
  message(FATAL_ERROR
    "Build the isolated ALAC decoder first: scripts/setup-airplay-codec.sh ${_codec_platform}")
endif()
add_library(cloudtune_airplay STATIC receiver.cpp crypto.cpp codec.cpp mdns.cpp)
target_include_directories(cloudtune_airplay PUBLIC
  "${_cloudtune_root}" "${_cloudtune_root}/include"
  PRIVATE "${CLOUDTUNE_FFMPEG_ROOT}/include" "${MBEDTLS_INCLUDE_DIR}")
target_link_libraries(cloudtune_airplay PRIVATE
  "${CLOUDTUNE_FFMPEG_ROOT}/lib/libavcodec.a"
  "${CLOUDTUNE_FFMPEG_ROOT}/lib/libavutil.a"
  "${MBEDCRYPTO_LIBRARY}" m Threads::Threads)
