cmake_minimum_required(VERSION 3.22.1)
project(crosslink_sdk_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LANVID_SDK_COMPONENTS AUDIO CACHE STRING "" FORCE)
set(LANVID_SDK_CORE_ONLY  ON    CACHE BOOL   "" FORCE)
set(LANVID_BUILD_SHARED   ON    CACHE BOOL   "" FORCE)
set(LANVID_BUILD_TESTS    OFF   CACHE BOOL   "" FORCE)

get_filename_component(LANVID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
add_subdirectory("${LANVID_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/crosslink_core_build")

add_library(crosslink_jni SHARED crosslink_sdk_jni.cpp)
target_include_directories(crosslink_jni PRIVATE
  "${LANVID_ROOT}/sdk/include"
)
target_compile_definitions(crosslink_jni PRIVATE CROSSLINK_USE_SHARED=1)
target_link_libraries(crosslink_jni PRIVATE
  crosslink_core_shared
  log
)
