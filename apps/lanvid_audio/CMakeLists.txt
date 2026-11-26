# Headless service library (Zero ImGui / GPU deps — reusable by tests)
add_library(lanvid_audio_service STATIC
  audio_service.cpp
)
target_include_directories(lanvid_audio_service PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/apps
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/sdk/include
)
target_link_libraries(lanvid_audio_service PUBLIC
  lanvid_json_config
  lanvid_core
)
set_target_properties(lanvid_audio_service PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

if(TARGET lanvid_common_ui)
  if(WIN32)
    add_executable(lanvid_audio WIN32 main.cpp audio_app.cpp)
    target_compile_definitions(lanvid_audio PRIVATE LANVID_GUI_WINMAIN=1)
  else()
    add_executable(lanvid_audio main.cpp audio_app.cpp)
  endif()

  target_link_libraries(lanvid_audio PRIVATE
    lanvid_audio_service
    lanvid_common_ui
  )
  set_target_properties(lanvid_audio PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  if(APPLE)
    set_target_properties(lanvid_audio PROPERTIES
      MACOSX_BUNDLE TRUE
      OUTPUT_NAME "LanVid Audio"
      MACOSX_BUNDLE_BUNDLE_NAME "LanVid Audio"
      MACOSX_BUNDLE_EXECUTABLE_NAME "LanVid Audio"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.lanvid.audio"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/apps/common_ui/macos/LanVidApp-Info.plist.in"
    )
  endif()
endif()
