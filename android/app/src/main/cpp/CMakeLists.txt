cmake_minimum_required(VERSION 3.22.1)
project(lanvid_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build only the LanVid Video + Stereo Audio shared SDK core (liblanvid_core.so, ARM64 NEON)
set(LANVID_SDK_COMPONENTS VIDEO CACHE STRING "" FORCE)
set(LANVID_ENABLE_NEON    ON    CACHE BOOL   "" FORCE)
set(LANVID_ENABLE_AVX2    OFF   CACHE BOOL   "" FORCE)
set(LANVID_BUILD_SHARED   ON    CACHE BOOL   "" FORCE)
set(LANVID_BUILD_TESTS    OFF   CACHE BOOL   "" FORCE)

get_filename_component(LANVID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_subdirectory("${LANVID_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/lanvid_core_build")

add_library(lanvid_jni SHARED lanvid_jni.cpp)
target_include_directories(lanvid_jni PRIVATE
  "${LANVID_ROOT}/sdk/include"
  "${LANVID_ROOT}/src/lanvid"
  "${LANVID_ROOT}/src"
)
target_compile_definitions(lanvid_jni PRIVATE LANVID_USE_SHARED=1)
target_link_libraries(lanvid_jni PRIVATE
  lanvid_core_shared
  android
  aaudio
  log
)
