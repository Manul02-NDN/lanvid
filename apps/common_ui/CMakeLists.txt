# Pure C++20 JSON config library (Zero GUI / GPU / ImGui dependencies)
add_library(lanvid_json_config STATIC
  json_config.cpp
)
target_include_directories(lanvid_json_config PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set_target_properties(lanvid_json_config PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# Common UI + Native GPU Window Host library (Metal on macOS, D3D11 on Windows, SDL2/GL3 on Linux)
set(LV_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(_LV_CAN_BUILD_COMMON_UI FALSE)
if(LANVID_BUILD_GUI AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS AND EXISTS "${LV_IMGUI_DIR}/imgui.cpp")
  if(APPLE OR WIN32)
    set(_LV_CAN_BUILD_COMMON_UI TRUE)
  elseif(LV_HAVE_SDL2)
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
      set(_LV_CAN_BUILD_COMMON_UI TRUE)
    endif()
  endif()
endif()

if(_LV_CAN_BUILD_COMMON_UI)
  set(LV_COMMON_UI_SOURCES
    theme.cpp
    qr_code.cpp
    ${LV_IMGUI_DIR}/imgui.cpp
    ${LV_IMGUI_DIR}/imgui_draw.cpp
    ${LV_IMGUI_DIR}/imgui_tables.cpp
    ${LV_IMGUI_DIR}/imgui_widgets.cpp
  )

  if(APPLE)
    list(APPEND LV_COMMON_UI_SOURCES
      host_macos_metal.mm
      ${LV_IMGUI_DIR}/backends/imgui_impl_osx.mm
      ${LV_IMGUI_DIR}/backends/imgui_impl_metal.mm
    )
  elseif(WIN32)
    list(APPEND LV_COMMON_UI_SOURCES
      host_win32_d3d11.cpp
      ${LV_IMGUI_DIR}/backends/imgui_impl_win32.cpp
      ${LV_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
  else()
    list(APPEND LV_COMMON_UI_SOURCES
      host_linux_sdl2.cpp
      ${LV_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
      ${LV_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
  endif()

  add_library(lanvid_common_ui STATIC ${LV_COMMON_UI_SOURCES})
  target_include_directories(lanvid_common_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src
    ${LV_IMGUI_DIR}
    ${LV_IMGUI_DIR}/backends
  )
  target_link_libraries(lanvid_common_ui PUBLIC
    lanvid_json_config
    lanvid_core
  )
  set_target_properties(lanvid_common_ui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )

  if(APPLE)
    target_link_libraries(lanvid_common_ui PUBLIC
      "-framework Cocoa"
      "-framework Metal"
      "-framework MetalKit"
      "-framework QuartzCore"
      "-framework GameController"
    )
  elseif(WIN32)
    target_link_libraries(lanvid_common_ui PUBLIC d3d11 dxgi dwmapi)
  else()
    target_link_libraries(lanvid_common_ui PUBLIC OpenGL::GL)
  endif()
endif()
