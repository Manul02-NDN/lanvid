cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  # Embed debug info in .obj (/Z7) on MSVC so parallel builds don't contend on mspdbsrv.exe.
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "" FORCE)
endif()

project(lanvid VERSION 2.4.0 LANGUAGES CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()
set(LANVID_SDK_VERSION "${PROJECT_VERSION}")
add_compile_definitions(
  LANVID_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  LANVID_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  LANVID_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  LANVID_VERSION_STRING="${PROJECT_VERSION}"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/CompilerFlags.cmake)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- build options
option(LANVID_SDK_CORE_ONLY "Build pure C ABI SDK core without OS capture/GUI dependencies" OFF)
option(LANVID_ENABLE_AVX2   "Enable runtime-dispatched x86_64 AVX2 codec kernels" ON)
option(LANVID_ENABLE_NEON   "Enable ARM64/Apple Silicon NEON codec kernels" ON)
option(LANVID_BUILD_SHARED  "Build lanvid_core shared C ABI library (DLL/.so/.dylib)" ON)
option(LANVID_BUILD_GUI     "Build LanVid Studio Desktop GUI (Dear ImGui)" ON)
option(LANVID_BUILD_CLI     "Build CLI sender/receiver executables" ON)
option(LANVID_BUILD_TESTS   "Build unit tests and benchmark" ON)
option(LANVID_BUILD_DRIVERS "Build OS virtual audio devices (ASIO DLL / CoreAudio HAL / JACK bridge)" ON)

set(LV_IS_X86 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$")
  set(LV_IS_X86 TRUE)
endif()

set(LV_IS_ARM64 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(LV_IS_ARM64 TRUE)
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  set(LV_IS_ARM64 TRUE)
  set(LV_IS_X86 FALSE)
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  set(LV_IS_ARM64 FALSE)
  set(LV_IS_X86 TRUE)
endif()

# ---------------------------------------------------------------- core sources
set(LV_CORE_SOURCES
  src/common/cpu_features.cpp
  src/common/crc32c.cpp
  src/common/image.cpp
  src/codec/color.cpp
  src/codec/dct.cpp
  src/codec/quant.cpp
  src/codec/huffman.cpp
  src/codec/entropy.cpp
  src/codec/slice_format.cpp
  src/codec/slice_codec.cpp
  src/codec/frame_codec.cpp
  src/codec/video_codec_common.cpp
  src/parallel/thread_pool.cpp
  src/audio/audio_jitter_buffer.cpp
  src/audio/master_clock.cpp
  src/audio/audio_sync_matrix.cpp
  src/audio/audio_tx.cpp
  src/audio/audio_rx.cpp
  src/net/packet.cpp
  src/net/packetizer.cpp
  src/net/assembler.cpp
  src/net/adaptive_quality.cpp
  src/net/sender.cpp
  src/net/receiver.cpp
  src/net/discovery.cpp
  src/core/lanvid_c_api.cpp
  src/audio/channel_patchbay.cpp
  src/audio/multi_device_matrix.cpp
  src/midi/midi_sync_engine.cpp
  src/midi/midi_device_manager.cpp
)

# Desktop-only: LanVidAudioBus_v1 shared-memory bridge to the OS virtual audio devices
# (Android bionic has no shm_open; mobile apps never host the virtual device).
if(NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  list(APPEND LV_CORE_SOURCES
    src/audio/shm_audio_bridge.cpp
    src/audio/shm_audio_host.cpp
  )
endif()

if(NOT LANVID_SDK_CORE_ONLY)
  list(APPEND LV_CORE_SOURCES
    src/app/test_pattern.cpp
    src/app/file_source.cpp
    src/app/stats.cpp
  )
endif()

# SIMD translation units
set(LV_SIMD_DEFS "")
if(LV_IS_X86 AND LANVID_ENABLE_AVX2 AND NOT ANDROID AND NOT IOS)
  list(APPEND LV_CORE_SOURCES
    src/codec/dct_avx2.cpp
    src/codec/color_avx2.cpp
  )
  list(APPEND LV_SIMD_DEFS LV_HAVE_AVX2_TU=1)
  if(MSVC)
    set_source_files_properties(
      src/codec/dct_avx2.cpp
      src/codec/color_avx2.cpp
      PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(
      src/codec/dct_avx2.cpp
      src/codec/color_avx2.cpp
      PROPERTIES COMPILE_OPTIONS "-mavx2")
  endif()
endif()

if(LV_IS_ARM64 AND LANVID_ENABLE_NEON)
  list(APPEND LV_CORE_SOURCES
    src/codec/dct_neon.cpp
    src/codec/color_neon.cpp
  )
  list(APPEND LV_SIMD_DEFS LV_HAVE_NEON_TU=1)
  if(NOT MSVC AND NOT APPLE)
    set_source_files_properties(
      src/codec/dct_neon.cpp
      src/codec/color_neon.cpp
      PROPERTIES COMPILE_OPTIONS "-march=armv8-a+simd")
  endif()
endif()

if(LANVID_SDK_CORE_ONLY)
  list(APPEND LV_SIMD_DEFS LANVID_SDK_CORE_ONLY=1)
endif()
if(NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  list(APPEND LV_SIMD_DEFS LV_HAVE_SHM_BUS=1)  # virtual-device bridge + full-duplex C ABI available
endif()

# Platform-specific network, preview, capture, and audio backends
set(LV_PLATFORM_LIBS Threads::Threads)
if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_library(LV_RT_LIB rt)  # shm_open on glibc < 2.34
  if(LV_RT_LIB)
    list(APPEND LV_PLATFORM_LIBS ${LV_RT_LIB})
  endif()
endif()
set(LV_PLATFORM_INCLUDES "")
set(LV_HAVE_SDL2 FALSE)
set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lpthread")

if(WIN32)
  list(APPEND LV_CORE_SOURCES
    src/net/udp_socket_win32.cpp
    src/codec/hw_codec_win32.cpp
  )
  # Core SDK always links Winsock2 + Windows Media Foundation + D3D11/DXGI for HW H.264/H.265 codecs
  list(APPEND LV_PLATFORM_LIBS ws2_32 iphlpapi ole32 oleaut32 uuid d3d11 dxgi mf mfplat mfuuid winmm)
  set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lws2_32 -liphlpapi -lole32 -loleaut32 -luuid -ld3d11 -ldxgi -lmf -lmfplat -lmfuuid")
  if(NOT LANVID_SDK_CORE_ONLY)
    list(APPEND LV_CORE_SOURCES
      src/platform/preview_window_win32.cpp
      src/platform/screen_capture_win32.cpp
      src/platform/camera_capture_win32.cpp
      src/platform/audio_capture_win32.cpp
      src/platform/audio_playback_win32.cpp
    )
    list(APPEND LV_PLATFORM_LIBS
      d3dcompiler gdi32 user32 avrt mmdevapi mfreadwrite
    )
  endif()
else()
  list(APPEND LV_CORE_SOURCES src/net/udp_socket_posix.cpp)
  if(APPLE)
    list(APPEND LV_CORE_SOURCES src/codec/hw_codec_apple.mm)
    list(APPEND LV_PLATFORM_LIBS
      "-framework VideoToolbox"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework CoreFoundation"
      "-framework CoreMIDI"
    )
    set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lpthread -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation")
    if(NOT IOS)  # CoreAudio HAL: System Audio slots of the Multi-Device Audio Matrix
      list(APPEND LV_PLATFORM_LIBS "-framework CoreAudio")
      string(APPEND LANVID_PKGCONFIG_PRIVATE_LIBS " -framework CoreAudio")
    endif()
  elseif(ANDROID)
    list(APPEND LV_CORE_SOURCES src/codec/hw_codec_android.cpp)
    list(APPEND LV_PLATFORM_LIBS log mediandk)
  else()
    list(APPEND LV_CORE_SOURCES src/codec/hw_codec_linux.cpp)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBAV QUIET libavcodec libavutil)
      if(LIBAV_FOUND)
        message(STATUS "lanvid: FFmpeg libavcodec found -> enabling Linux H.264/H.265 codec backend")
        list(APPEND LV_SIMD_DEFS LV_HAVE_FFMPEG=1)
        list(APPEND LV_PLATFORM_INCLUDES ${LIBAV_INCLUDE_DIRS})
        list(APPEND LV_PLATFORM_LIBS ${LIBAV_LIBRARIES})
        set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lpthread -lavcodec -lavutil")
      else()
        message(STATUS "lanvid: FFmpeg libavcodec not found -> Linux H.264/H.265 disabled (LanVid DCT active)")
      endif()
    endif()
  endif()
  if(NOT LANVID_SDK_CORE_ONLY)
    list(APPEND LV_CORE_SOURCES src/platform/platform_stubs_unix.cpp)
    if(ANDROID OR IOS)
      list(APPEND LV_CORE_SOURCES src/platform/preview_window_stub.cpp)
    else()
      find_package(SDL2 QUIET)
      if(SDL2_FOUND)
        set(LV_HAVE_SDL2 TRUE)
        message(STATUS "lanvid: SDL2 found -> enabling SDL2 preview & GUI backend")
        list(APPEND LV_CORE_SOURCES src/platform/preview_window_sdl2.cpp)
        if(TARGET SDL2::SDL2)
          list(APPEND LV_PLATFORM_LIBS SDL2::SDL2)
        else()
          list(APPEND LV_PLATFORM_INCLUDES ${SDL2_INCLUDE_DIRS})
          list(APPEND LV_PLATFORM_LIBS ${SDL2_LIBRARIES})
        endif()
      else()
        message(STATUS "lanvid: SDL2 not found -> building UNIX headless preview stub")
        list(APPEND LV_CORE_SOURCES src/platform/preview_window_stub.cpp)
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------- static core
add_library(lanvid_core STATIC ${LV_CORE_SOURCES})
add_library(LanVidSDK::CoreStatic ALIAS lanvid_core)
target_include_directories(lanvid_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  ${LV_PLATFORM_INCLUDES}
)
if(LV_SIMD_DEFS)
  target_compile_definitions(lanvid_core PRIVATE ${LV_SIMD_DEFS})
endif()
target_link_libraries(lanvid_core PUBLIC ${LV_PLATFORM_LIBS})
lanvid_set_flags(lanvid_core)

# ---------------------------------------------------------------- shared C ABI SDK library (DLL/.so/.dylib)
if(LANVID_BUILD_SHARED)
  add_library(lanvid_core_shared SHARED ${LV_CORE_SOURCES})
  add_library(LanVidSDK::Core ALIAS lanvid_core_shared)
  set_target_properties(lanvid_core_shared PROPERTIES
    OUTPUT_NAME "lanvid_core"
    EXPORT_NAME "Core"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  if(WIN32)
    set_target_properties(lanvid_core_shared PROPERTIES ARCHIVE_OUTPUT_NAME "lanvid_core_dll")
  endif()
  target_include_directories(lanvid_core_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    ${LV_PLATFORM_INCLUDES}
  )
  target_compile_definitions(lanvid_core_shared
    PRIVATE LANVID_BUILD_SHARED ${LV_SIMD_DEFS}
    INTERFACE LANVID_USE_SHARED=1
  )
  target_link_libraries(lanvid_core_shared PRIVATE ${LV_PLATFORM_LIBS})
  lanvid_set_flags(lanvid_core_shared)
endif()

# ---------------------------------------------------------------- Desktop GUI (LanVid Studio)
set(LV_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
if(LANVID_BUILD_GUI AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS AND EXISTS "${LV_IMGUI_DIR}/imgui.cpp")
  set(LV_IMGUI_CORE
    ${LV_IMGUI_DIR}/imgui.cpp
    ${LV_IMGUI_DIR}/imgui_draw.cpp
    ${LV_IMGUI_DIR}/imgui_tables.cpp
    ${LV_IMGUI_DIR}/imgui_widgets.cpp
  )
  set(LV_GUI_SOURCES
    src/gui/app_config.cpp
    src/gui/lanvid_studio.cpp
    ${LV_IMGUI_CORE}
  )

  if(WIN32)
    add_executable(lanvid_app WIN32
      src/gui/main_win32.cpp
      ${LV_GUI_SOURCES}
      ${LV_IMGUI_DIR}/backends/imgui_impl_win32.cpp
      ${LV_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_compile_definitions(lanvid_app PRIVATE LANVID_GUI_WINMAIN=1)
    target_include_directories(lanvid_app PRIVATE ${LV_IMGUI_DIR} ${LV_IMGUI_DIR}/backends)
    target_link_libraries(lanvid_app PRIVATE lanvid_core dwmapi)
    lanvid_set_flags(lanvid_app)
  elseif(LV_HAVE_SDL2)
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
      add_executable(lanvid_app
        src/gui/main_linux_sdl2.cpp
        ${LV_GUI_SOURCES}
        ${LV_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${LV_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
      )
      target_include_directories(lanvid_app PRIVATE ${LV_IMGUI_DIR} ${LV_IMGUI_DIR}/backends)
      target_link_libraries(lanvid_app PRIVATE lanvid_core OpenGL::GL)
      lanvid_set_flags(lanvid_app)
      if(APPLE)
        # macOS: "LanVid Studio.app" (Retina, microphone permission prompt for CoreAudio inputs).
        set_target_properties(lanvid_app PROPERTIES
          MACOSX_BUNDLE TRUE
          OUTPUT_NAME "LanVid Studio"
          MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/macos/LanVidStudio-Info.plist.in")
        target_compile_definitions(lanvid_app PRIVATE GL_SILENCE_DEPRECATION=1)
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------- CLI executables
if(LANVID_BUILD_CLI AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  add_executable(sender src/app/sender_main.cpp src/app/duplex_cli.cpp)
  target_link_libraries(sender PRIVATE lanvid_core)
  lanvid_set_flags(sender)

  add_executable(receiver src/app/receiver_main.cpp)
  target_link_libraries(receiver PRIVATE lanvid_core)
  lanvid_set_flags(receiver)
endif()

# ---------------------------------------------------------------- tests & benchmark

# ---------------------------------------------------------------- virtual 64ch OS audio devices
# Each driver compiles the self-contained shared-memory bus directly (no lanvid_core dependency),
# because it is loaded into foreign processes (DAWs / coreaudiod).
if(LANVID_BUILD_DRIVERS AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  if(APPLE)
    add_library(LanVidAudio MODULE
      drivers/macos/hal/LanVidAudioDriver.cpp
      src/audio/shm_audio_bridge.cpp)
    set_target_properties(LanVidAudio PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION driver
      OUTPUT_NAME LanVidAudio
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/drivers/macos/hal/Info.plist.in"
      OSX_ARCHITECTURES "arm64;x86_64"
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(LanVidAudio PRIVATE src)
    target_compile_options(LanVidAudio PRIVATE -Wall -Wextra -O2)
    target_link_libraries(LanVidAudio PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
  elseif(WIN32)
    add_library(LanVidASIO SHARED
      drivers/windows/asio/lanvid_asio.cpp
      drivers/windows/asio/lanvid_asio.def
      src/audio/shm_audio_bridge.cpp)
    target_include_directories(LanVidASIO PRIVATE src drivers/windows/asio)
    target_link_libraries(LanVidASIO PRIVATE winmm advapi32 ole32 user32)
    # Static CRT: the DLL is loaded into arbitrary DAW processes.
    set_target_properties(LanVidASIO PROPERTIES OUTPUT_NAME LanVidASIO
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    add_executable(lanvid-audio-bridge
      drivers/linux/jack_pw/lanvid_audio_bridge.cpp
      src/audio/shm_audio_bridge.cpp)
    target_include_directories(lanvid-audio-bridge PRIVATE src)
    target_link_libraries(lanvid-audio-bridge PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    find_library(LV_RT_LIB rt)
    if(LV_RT_LIB)
      target_link_libraries(lanvid-audio-bridge PRIVATE ${LV_RT_LIB})
    endif()
  endif()
endif()

if(LANVID_BUILD_TESTS AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  enable_testing()
  add_executable(lanvid_tests
    tests/test_main.cpp
    tests/test_dct.cpp
    tests/test_huffman.cpp
    tests/test_codec_roundtrip.cpp
    tests/test_parallel.cpp
    tests/test_packet.cpp
    tests/test_assembler.cpp
    tests/test_fec.cpp
    tests/test_audio.cpp
    tests/test_audio_64ch.cpp
    tests/test_shm_audio_bus.cpp
    tests/test_hal_driver.cpp
    tests/test_c_api.cpp
    tests/test_video_codecs.cpp
    tests/test_audio_duplex.cpp
    tests/test_multi_device_matrix.cpp
    tests/test_coreaudio_matrix.cpp
    tests/test_midi_sync.cpp
    tests/test_coremidi_ports.cpp
    src/gui/app_config.cpp
  )
  target_link_libraries(lanvid_tests PRIVATE lanvid_core)
  if(NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
    target_compile_definitions(lanvid_tests PRIVATE LV_HAVE_SHM_BUS=1)
  endif()
  if(TARGET LanVidAudio)
    add_dependencies(lanvid_tests LanVidAudio)
    target_compile_definitions(lanvid_tests PRIVATE
      LV_HAL_DRIVER_BINARY="$<TARGET_FILE:LanVidAudio>")
  endif()
  lanvid_set_flags(lanvid_tests)
  add_test(NAME lanvid_tests COMMAND lanvid_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  add_executable(bench_codec bench/bench_codec.cpp)
  target_link_libraries(bench_codec PRIVATE lanvid_core)
  lanvid_set_flags(bench_codec)
endif()

# ---------------------------------------------------------------- SDK CMake Install & pkg-config
if(NOT ANDROID AND NOT IOS)
  set(LV_INSTALL_TARGETS lanvid_core)
  set_target_properties(lanvid_core PROPERTIES EXPORT_NAME "CoreStatic")
  if(TARGET lanvid_core_shared)
    list(APPEND LV_INSTALL_TARGETS lanvid_core_shared)
  endif()

  install(TARGETS ${LV_INSTALL_TARGETS}
    EXPORT LanVidSDKTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  install(DIRECTORY sdk/include/lanvid
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(FILES sdk/cpp/lanvid.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lanvid
  )

  install(EXPORT LanVidSDKTargets
    FILE LanVidSDKTargets.cmake
    NAMESPACE LanVidSDK::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanVidSDK
  )

  configure_package_config_file(
    sdk/cmake/LanVidSDKConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/LanVidSDKConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanVidSDK
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LanVidSDKConfigVersion.cmake"
    VERSION ${LANVID_SDK_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LanVidSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LanVidSDKConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanVidSDK
  )

  configure_file(
    sdk/cmake/lanvid-sdk.pc.in
    "${CMAKE_CURRENT_BINARY_DIR}/lanvid-sdk.pc"
    @ONLY
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/lanvid-sdk.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )
endif()
