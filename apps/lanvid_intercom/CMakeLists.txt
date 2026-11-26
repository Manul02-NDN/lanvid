add_library(lanvid_intercom_service STATIC
  intercom_service.cpp
)
target_include_directories(lanvid_intercom_service PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/apps
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/sdk/include
)
target_link_libraries(lanvid_intercom_service PUBLIC
  lanvid_json_config
  lanvid_core
)
set_target_properties(lanvid_intercom_service PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

if(TARGET lanvid_common_ui)
  if(WIN32)
    add_executable(lanvid_intercom WIN32 main.cpp intercom_app.cpp)
    target_compile_definitions(lanvid_intercom PRIVATE LANVID_GUI_WINMAIN=1)
  else()
    add_executable(lanvid_intercom main.cpp intercom_app.cpp)
  endif()

  target_link_libraries(lanvid_intercom PRIVATE
    lanvid_intercom_service
    lanvid_common_ui
  )
  set_target_properties(lanvid_intercom PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  if(APPLE)
    set_target_properties(lanvid_intercom PROPERTIES
      MACOSX_BUNDLE TRUE
      OUTPUT_NAME "LanVid Intercom"
      MACOSX_BUNDLE_BUNDLE_NAME "LanVid Intercom"
      MACOSX_BUNDLE_EXECUTABLE_NAME "LanVid Intercom"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.lanvid.intercom"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/apps/common_ui/macos/LanVidApp-Info.plist.in"
    )
  endif()
endif()
