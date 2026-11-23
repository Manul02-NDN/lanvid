cmake_minimum_required(VERSION 3.22.1)
project(lanvid_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LanVid core as a static C ABI library (lv_monitor_client_*), linked into one JNI .so.
set(LANVID_SDK_CORE_ONLY ON  CACHE BOOL "" FORCE)
set(LANVID_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(LANVID_BUILD_GUI     OFF CACHE BOOL "" FORCE)
set(LANVID_BUILD_CLI     OFF CACHE BOOL "" FORCE)
set(LANVID_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(LANVID_ENABLE_AVX2   OFF CACHE BOOL "" FORCE)
if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(LANVID_ENABLE_NEON ON CACHE BOOL "" FORCE)
else()
  set(LANVID_ENABLE_NEON OFF CACHE BOOL "" FORCE)
endif()

get_filename_component(LANVID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
add_subdirectory("${LANVID_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/lanvid_core_build")

add_library(lanvid_monitor SHARED monitor_jni.cpp)
target_include_directories(lanvid_monitor PRIVATE "${LANVID_ROOT}/sdk/include")
target_link_libraries(lanvid_monitor PRIVATE lanvid_core aaudio log android)
