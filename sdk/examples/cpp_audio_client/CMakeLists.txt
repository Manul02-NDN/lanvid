cmake_minimum_required(VERSION 3.20)
project(crosslink_audio_client_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CrossLinkSDK REQUIRED)

add_executable(crosslink_audio_app main.cpp)
target_link_libraries(crosslink_audio_app PRIVATE CrossLinkSDK::Audio)

if(WIN32)
  add_custom_command(TARGET crosslink_audio_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:CrossLinkSDK::Audio>
      $<TARGET_FILE_DIR:crosslink_audio_app>
  )
endif()
