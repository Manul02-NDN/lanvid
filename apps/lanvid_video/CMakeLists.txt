add_library(lanvid_video_service STATIC
  video_service.cpp
  hub/camera_slot.cpp
  hub/hub_discovery.cpp
  hub/iso_recorder.cpp
  hub/vcam_bus.cpp
  hub/tally_intercom_hub.cpp
  hub/video_hub_engine.cpp
)
target_include_directories(lanvid_video_service PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/apps
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/sdk/include
)
target_link_libraries(lanvid_video_service PUBLIC
  lanvid_json_config
  lanvid_core
)
set_target_properties(lanvid_video_service PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

if(TARGET lanvid_common_ui)
  if(WIN32)
    add_executable(lanvid_video WIN32 main.cpp video_app.cpp)
    target_compile_definitions(lanvid_video PRIVATE LANVID_GUI_WINMAIN=1)
  else()
    add_executable(lanvid_video main.cpp video_app.cpp)
  endif()

  target_link_libraries(lanvid_video PRIVATE
    lanvid_video_service
    lanvid_common_ui
  )
  set_target_properties(lanvid_video PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  if(APPLE)
    set_target_properties(lanvid_video PROPERTIES
      MACOSX_BUNDLE TRUE
      OUTPUT_NAME "LanVid Video"
      MACOSX_BUNDLE_BUNDLE_NAME "LanVid Video"
      MACOSX_BUNDLE_EXECUTABLE_NAME "LanVid Video"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.lanvid.video"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/apps/common_ui/macos/LanVidApp-Info.plist.in"
    )
  endif()
endif()
