cmake_minimum_required(VERSION 3.20)
project(lanvid_crosslink_combined_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LanVidSDK REQUIRED)
find_package(CrossLinkSDK REQUIRED)

# Proves zero symbol collisions when linking BOTH shared libraries into one executable
add_executable(combined_shared main.cpp)
target_link_libraries(combined_shared PRIVATE LanVidSDK::Video CrossLinkSDK::Audio)

# Proves zero duplicate symbols when linking BOTH static archives (including isolated lv_sys/cl_sys & Opus)
add_executable(combined_static main.cpp)
target_link_libraries(combined_static PRIVATE LanVidSDK::VideoStatic CrossLinkSDK::AudioStatic)

if(WIN32)
  add_custom_command(TARGET combined_shared POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:LanVidSDK::Video>
      $<TARGET_FILE:CrossLinkSDK::Audio>
      $<TARGET_FILE_DIR:combined_shared>
  )
endif()
