cmake_minimum_required(VERSION 3.20)
project(lanvid_video_client_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LanVidSDK REQUIRED)

add_executable(lanvid_video_app main.cpp)
target_link_libraries(lanvid_video_app PRIVATE LanVidSDK::Video)

if(WIN32)
  add_custom_command(TARGET lanvid_video_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:LanVidSDK::Video>
      $<TARGET_FILE_DIR:lanvid_video_app>
  )
endif()
