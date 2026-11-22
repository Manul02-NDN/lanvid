cmake_minimum_required(VERSION 3.20)
project(lanvid_sdk_client_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LanVidSDK REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE LanVidSDK::Core)

if(WIN32)
  # Copy lanvid_core.dll alongside my_app.exe for effortless local execution
  add_custom_command(TARGET my_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:LanVidSDK::Core>
      $<TARGET_FILE_DIR:my_app>
  )
endif()
