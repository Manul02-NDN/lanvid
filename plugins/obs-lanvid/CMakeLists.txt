cmake_minimum_required(VERSION 3.20)
project(obs-lanvid VERSION 2.6.0 LANGUAGES CXX)

# 1. Core library (always built when obs-lanvid is enabled; used by both the OBS plugin and headless tests)
add_library(obs_lanvid_core STATIC
  src/obs_lanvid_core.cpp
)
target_include_directories(obs_lanvid_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(obs_lanvid_core PUBLIC lanvid_core)
if(COMMAND lanvid_set_flags)
  lanvid_set_flags(obs_lanvid_core)
endif()

# 2. Native OBS Studio plugin module (built automatically when libobs + obs-frontend-api are available)
find_package(libobs QUIET)
find_package(obs-frontend-api QUIET)
if(NOT (libobs_FOUND AND obs-frontend-api_FOUND))
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ObsSdkImport.cmake)  # light SDK: headers + installed OBS binaries
endif()
find_package(Qt6 COMPONENTS Widgets QUIET)

if(libobs_FOUND AND obs-frontend-api_FOUND)
  message(STATUS "obs-lanvid: libobs & obs-frontend-api found -> building native OBS Studio plugin")
  add_library(obs-lanvid MODULE
    src/plugin_main.cpp
    src/obs_lanvid_source.cpp
    src/obs_lanvid_output.cpp
    src/obs_lanvid_dock.cpp
  )
  target_compile_definitions(obs-lanvid PRIVATE LV_HAVE_REAL_LIBOBS=1)
  target_link_libraries(obs-lanvid PRIVATE obs_lanvid_core OBS::libobs OBS::obs-frontend-api)

  if(Qt6Widgets_FOUND)
    message(STATUS "obs-lanvid: Qt6::Widgets found -> enabling LanVid Control & Tally Dock")
    target_compile_definitions(obs-lanvid PRIVATE LV_HAVE_QT6_WIDGETS=1)
    target_link_libraries(obs-lanvid PRIVATE Qt6::Widgets)
  endif()

  if(APPLE)
    set_target_properties(obs-lanvid PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      PREFIX ""
      OUTPUT_NAME "obs-lanvid"
    )
    install(TARGETS obs-lanvid LIBRARY DESTINATION "obs-studio/plugins")
    install(DIRECTORY data/locale DESTINATION "obs-studio/plugins/obs-lanvid.plugin/Contents/Resources")
  elseif(WIN32)
    set_target_properties(obs-lanvid PROPERTIES PREFIX "" OUTPUT_NAME "obs-lanvid")
    install(TARGETS obs-lanvid RUNTIME DESTINATION "obs-plugins/64bit" LIBRARY DESTINATION "obs-plugins/64bit")
    install(DIRECTORY data/locale DESTINATION "data/obs-plugins/obs-lanvid")
  else()
    set_target_properties(obs-lanvid PROPERTIES PREFIX "" OUTPUT_NAME "obs-lanvid")
    install(TARGETS obs-lanvid LIBRARY DESTINATION "lib/obs-plugins")
    install(DIRECTORY data/locale DESTINATION "share/obs/obs-plugins/obs-lanvid")
  endif()
else()
  message(STATUS "obs-lanvid: libobs/obs-frontend-api not found -> built obs_lanvid_core for headless tests (install OBS SDK to build obs-lanvid module)")
endif()
