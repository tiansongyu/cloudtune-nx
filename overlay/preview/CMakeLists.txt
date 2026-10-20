# SPDX-License-Identifier: GPL-2.0-or-later
# The GPLv2 overlay and GPLv3 engine remain separate executables, as on Switch.
find_package(SDL2 REQUIRED)
add_executable(cloudtune-overlay-preview ../source/main.cpp runtime.cpp client.cpp)
target_include_directories(cloudtune-overlay-preview PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/overlay/vendor/libtesla")
target_compile_definitions(cloudtune-overlay-preview PRIVATE
  CLOUDTUNE_PREVIEW_FONT="${PROJECT_SOURCE_DIR}/resources/font/chinese_fallback.ttf")
target_link_libraries(cloudtune-overlay-preview PRIVATE SDL2::SDL2)

add_executable(cloudtune-preview-service
  "${PROJECT_SOURCE_DIR}/background/tests/overlay_preview_service.cpp")
target_include_directories(cloudtune-preview-service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cloudtune-preview-service PRIVATE ncm_engine)
