cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  # Embed debug info in .obj (/Z7) on MSVC so parallel builds don't contend on mspdbsrv.exe.
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded CACHE STRING "" FORCE)
endif()

project(lanvid VERSION 3.0.0 LANGUAGES CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

set(LANVID_SDK_VERSION    "3.0.0")
set(LANVID_SDK_VER_MAJOR  3)
set(LANVID_SDK_VER_MINOR  0)
set(LANVID_SDK_VER_PATCH  0)

set(CROSSLINK_SDK_VERSION   "1.0.0")
set(CROSSLINK_SDK_VER_MAJOR 1)
set(CROSSLINK_SDK_VER_MINOR 0)
set(CROSSLINK_SDK_VER_PATCH 0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(cmake/CompilerFlags.cmake)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- Selectable SDK protocol components
# Three install / build modes:
#   ALL   — build & install both LanVid (Video + Stereo Audio) and CrossLink (64ch AoIP + MIDI + Grid)
#   VIDEO — build & install ONLY LanVid Video + Stereo Audio SDK
#   AUDIO — build & install ONLY CrossLink Audio / MIDI / Grid SDK
set(LANVID_SDK_COMPONENTS "ALL" CACHE STRING
  "SDK protocols to build/install: ALL, VIDEO (LanVid), AUDIO (CrossLink)")
set_property(CACHE LANVID_SDK_COMPONENTS PROPERTY STRINGS ALL VIDEO AUDIO)
string(TOUPPER "${LANVID_SDK_COMPONENTS}" _SDK_COMP_UPPER)

if(NOT _SDK_COMP_UPPER MATCHES "^(ALL|VIDEO|AUDIO)$")
  message(FATAL_ERROR
    "Invalid LANVID_SDK_COMPONENTS='${LANVID_SDK_COMPONENTS}'. Expected one of: ALL, VIDEO, AUDIO.")
endif()

option(LANVID_BUILD_VIDEO    "Build LanVid Video + Stereo Audio SDK (when LANVID_SDK_COMPONENTS=ALL)" ON)
option(CROSSLINK_BUILD_AUDIO "Build CrossLink Audio/MIDI/Grid SDK (when LANVID_SDK_COMPONENTS=ALL)" ON)

if(_SDK_COMP_UPPER STREQUAL "VIDEO")
  set(_ENABLE_LANVID    ON)
  set(_ENABLE_CROSSLINK OFF)
elseif(_SDK_COMP_UPPER STREQUAL "AUDIO")
  set(_ENABLE_LANVID    OFF)
  set(_ENABLE_CROSSLINK ON)
else()
  set(_ENABLE_LANVID    ${LANVID_BUILD_VIDEO})
  set(_ENABLE_CROSSLINK ${CROSSLINK_BUILD_AUDIO})
endif()

if(NOT _ENABLE_LANVID AND NOT _ENABLE_CROSSLINK)
  message(FATAL_ERROR "At least one protocol (LanVid VIDEO or CrossLink AUDIO) must be enabled.")
endif()

message(STATUS "SDK Components [${_SDK_COMP_UPPER}]: LanVid(Video v${LANVID_SDK_VERSION})=${_ENABLE_LANVID}, CrossLink(Audio v${CROSSLINK_SDK_VERSION})=${_ENABLE_CROSSLINK}")

# ---------------------------------------------------------------- Shared build options
option(LANVID_SDK_CORE_ONLY "Build pure C ABI SDK core without OS capture/file-source extras" OFF)
option(LANVID_ENABLE_AVX2   "Enable runtime-dispatched x86_64 AVX2 codec kernels (LanVid)" ON)
option(LANVID_ENABLE_NEON   "Enable ARM64/Apple Silicon NEON codec kernels (LanVid)" ON)
option(LANVID_BUILD_SHARED  "Build shared C ABI SDK libraries (DLL/.so/.dylib)" ON)
option(LANVID_BUILD_TESTS   "Build unit test suites for enabled SDK protocols" ON)

# macOS: POSIX shared-memory name for LanVidVideoBus_v1 ("<group>/vbus1", <= 31 chars)
set(LANVID_MACOS_APP_GROUP "9SN7V4SC2S.lanvid" CACHE STRING
  "App Group of the LanVid macOS Camera Extension (<TEAM_ID>.lanvid)")

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

# ---------------------------------------------------------------- libopus (CELT 2.5 ms low-latency)
# Used by CrossLink for compressed AoIP/Monitor modes, and by LanVid strictly for Intercom VOIP.
set(_OPUS_FROM_FETCHCONTENT OFF)
find_package(Opus QUIET)
if(NOT Opus_FOUND)
  set(_OPUS_FROM_FETCHCONTENT ON)
  set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(opus)
  if(TARGET opus AND NOT TARGET Opus::opus)
    add_library(Opus::opus ALIAS opus)
  endif()
  if(TARGET opus)
    # PIC required for linking into shared SDK libraries; hidden C visibility ensures
    # neither liblanvid_core nor libcrosslink_core leaks opus_* symbols dynamically.
    set_target_properties(opus PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      C_VISIBILITY_PRESET hidden
    )
  endif()
endif()

# ---------------------------------------------------------------- Protocol-neutral common primitives
# Compiled independently into each protocol library under isolated namespaces
# (lv_sys for LanVid, cl_sys for CrossLink) -> ZERO shared state and ZERO duplicate symbols.
set(COMMON_NET_SOURCES
  src/common/cpu_features.cpp
  src/common/rt_thread.cpp
  src/common/crc32c.cpp
  src/common/network_interfaces.cpp
)
if(WIN32)
  list(APPEND COMMON_NET_SOURCES src/common/udp_socket_win32.cpp)
else()
  list(APPEND COMMON_NET_SOURCES src/common/udp_socket_posix.cpp)
endif()

set(COMMON_RT_LIB "")
if(UNIX AND NOT APPLE AND NOT ANDROID)
  find_library(_RT_LIB rt)  # shm_open on glibc < 2.34
  if(_RT_LIB)
    set(COMMON_RT_LIB ${_RT_LIB})
  endif()
endif()

if(LANVID_BUILD_TESTS AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS)
  enable_testing()
endif()

# ==============================================================================
# PROTOCOL 1: LanVid Video + Stereo Audio SDK (v3.0.0)
# ==============================================================================
if(_ENABLE_LANVID)
  set(LV_CORE_SOURCES
    ${COMMON_NET_SOURCES}
    src/lanvid/common/image.cpp
    src/lanvid/codec/color.cpp
    src/lanvid/codec/dct.cpp
    src/lanvid/codec/quant.cpp
    src/lanvid/codec/huffman.cpp
    src/lanvid/codec/entropy.cpp
    src/lanvid/codec/slice_format.cpp
    src/lanvid/codec/slice_codec.cpp
    src/lanvid/codec/frame_codec.cpp
    src/lanvid/codec/video_codec_common.cpp
    src/lanvid/parallel/thread_pool.cpp
    src/lanvid/audio/stereo_audio_buffer.cpp
    src/lanvid/audio/intercom_engine.cpp
    src/lanvid/net/packet.cpp
    src/lanvid/net/packetizer.cpp
    src/lanvid/net/assembler.cpp
    src/lanvid/net/adaptive_quality.cpp
    src/lanvid/net/video_sender.cpp
    src/lanvid/net/video_receiver.cpp
    src/lanvid/net/video_discovery.cpp
    src/lanvid/video/video_bus.cpp
    src/lanvid/video/video_playout_buffer.cpp
    src/lanvid/video/iso_recorder.cpp
    src/lanvid/video/multicam_manager.cpp
    src/lanvid/core/lanvid_c_api.cpp
  )

  if(NOT LANVID_SDK_CORE_ONLY)
    list(APPEND LV_CORE_SOURCES
      src/lanvid/sources/test_pattern.cpp
      src/lanvid/sources/file_source.cpp
    )
  endif()

  set(LV_PRIVATE_DEFS
    NET_COMMON_NS=lv_sys
    NET_COMMON_IMPORT_NS=lv
    NET_SIMD_ENV_VAR="LV_SIMD"
    LANVID_VERSION_MAJOR=${LANVID_SDK_VER_MAJOR}
    LANVID_VERSION_MINOR=${LANVID_SDK_VER_MINOR}
    LANVID_VERSION_PATCH=${LANVID_SDK_VER_PATCH}
    LANVID_VERSION_STRING="${LANVID_SDK_VERSION}"
    LV_HAVE_OPUS=1
  )
  if(APPLE AND NOT IOS)
    list(APPEND LV_PRIVATE_DEFS LANVID_VIDEO_BUS_NAME="${LANVID_MACOS_APP_GROUP}/vbus1")
  endif()
  if(LANVID_SDK_CORE_ONLY)
    list(APPEND LV_PRIVATE_DEFS LANVID_SDK_CORE_ONLY=1)
  endif()

  # Video SIMD translation units
  if(LV_IS_X86 AND LANVID_ENABLE_AVX2 AND NOT ANDROID AND NOT IOS)
    list(APPEND LV_CORE_SOURCES
      src/lanvid/codec/dct_avx2.cpp
      src/lanvid/codec/color_avx2.cpp
    )
    list(APPEND LV_PRIVATE_DEFS LV_HAVE_AVX2_TU=1)
    if(MSVC)
      set_source_files_properties(
        src/lanvid/codec/dct_avx2.cpp
        src/lanvid/codec/color_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
      set_source_files_properties(
        src/lanvid/codec/dct_avx2.cpp
        src/lanvid/codec/color_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
  endif()

  if(LV_IS_ARM64 AND LANVID_ENABLE_NEON)
    list(APPEND LV_CORE_SOURCES
      src/lanvid/codec/dct_neon.cpp
      src/lanvid/codec/color_neon.cpp
    )
    list(APPEND LV_PRIVATE_DEFS LV_HAVE_NEON_TU=1)
    if(NOT MSVC AND NOT APPLE)
      set_source_files_properties(
        src/lanvid/codec/dct_neon.cpp
        src/lanvid/codec/color_neon.cpp
        PROPERTIES COMPILE_OPTIONS "-march=armv8-a+simd")
    endif()
  endif()

  # LanVid platform video/HW codec libs (ZERO CoreAudio / CoreMIDI!)
  set(LV_PLATFORM_LIBS Threads::Threads)
  set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lpthread")
  if(COMMON_RT_LIB)
    list(APPEND LV_PLATFORM_LIBS ${COMMON_RT_LIB})
    string(APPEND LANVID_PKGCONFIG_PRIVATE_LIBS " -lrt")
  endif()
  set(LV_PLATFORM_INCLUDES "")

  if(WIN32)
    list(APPEND LV_CORE_SOURCES src/lanvid/codec/hw_codec_win32.cpp)
    list(APPEND LV_PLATFORM_LIBS ws2_32 iphlpapi ole32 oleaut32 uuid d3d11 dxgi mf mfplat mfuuid winmm avrt)
    set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lws2_32 -liphlpapi -lole32 -loleaut32 -luuid -ld3d11 -ldxgi -lmf -lmfplat -lmfuuid -lwinmm -lavrt")
    if(NOT LANVID_SDK_CORE_ONLY)
      list(APPEND LV_CORE_SOURCES
        src/lanvid/platform/screen_capture_win32.cpp
        src/lanvid/platform/camera_capture_win32.cpp
        src/lanvid/platform/audio_capture_win32.cpp
        src/lanvid/platform/audio_playback_win32.cpp
      )
      list(APPEND LV_PLATFORM_LIBS gdi32 user32 mmdevapi mfreadwrite)
    endif()
  else()
    if(APPLE)
      list(APPEND LV_CORE_SOURCES src/lanvid/codec/hw_codec_apple.mm)
      list(APPEND LV_PLATFORM_LIBS
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation"
      )
      set(LANVID_PKGCONFIG_PRIVATE_LIBS "-lpthread -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation")
      if(NOT IOS)
        list(APPEND LV_PLATFORM_LIBS "-framework SystemConfiguration")
        string(APPEND LANVID_PKGCONFIG_PRIVATE_LIBS " -framework SystemConfiguration")
      endif()
    elseif(ANDROID)
      list(APPEND LV_CORE_SOURCES src/lanvid/codec/hw_codec_android.cpp)
      list(APPEND LV_PLATFORM_LIBS log mediandk)
    else()
      list(APPEND LV_CORE_SOURCES src/lanvid/codec/hw_codec_linux.cpp)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(LIBAV QUIET libavcodec libavutil)
        if(LIBAV_FOUND)
          message(STATUS "lanvid: FFmpeg libavcodec found -> enabling Linux H.264/H.265 codec backend")
          list(APPEND LV_PRIVATE_DEFS LV_HAVE_FFMPEG=1)
          list(APPEND LV_PLATFORM_INCLUDES ${LIBAV_INCLUDE_DIRS})
          list(APPEND LV_PLATFORM_LIBS ${LIBAV_LIBRARIES})
          string(APPEND LANVID_PKGCONFIG_PRIVATE_LIBS " -lavcodec -lavutil")
        else()
          message(STATUS "lanvid: FFmpeg libavcodec not found -> Linux H.264/H.265 disabled (LanVid DCT active)")
        endif()
      endif()
    endif()
    if(NOT LANVID_SDK_CORE_ONLY)
      list(APPEND LV_CORE_SOURCES src/lanvid/platform/platform_stubs_unix.cpp)
    endif()
  endif()

  # Static LanVid core
  add_library(lanvid_core STATIC ${LV_CORE_SOURCES})
  add_library(LanVidSDK::VideoStatic ALIAS lanvid_core)
  set_target_properties(lanvid_core PROPERTIES EXPORT_NAME "VideoStatic")
  target_include_directories(lanvid_core
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lanvid
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${LV_PLATFORM_INCLUDES}
  )
  target_compile_definitions(lanvid_core PRIVATE ${LV_PRIVATE_DEFS})
  target_link_libraries(lanvid_core PUBLIC ${LV_PLATFORM_LIBS} $<BUILD_INTERFACE:Opus::opus>)
  lanvid_set_flags(lanvid_core)

  # Shared LanVid C ABI SDK library (DLL/.so/.dylib)
  if(LANVID_BUILD_SHARED)
    add_library(lanvid_core_shared SHARED ${LV_CORE_SOURCES})
    add_library(LanVidSDK::Video ALIAS lanvid_core_shared)
    set_target_properties(lanvid_core_shared PROPERTIES
      OUTPUT_NAME "lanvid_core"
      EXPORT_NAME "Video"
      VERSION ${LANVID_SDK_VERSION}
      SOVERSION ${LANVID_SDK_VER_MAJOR}
      CXX_VISIBILITY_PRESET hidden
      OBJCXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
    )
    if(APPLE)
      target_link_options(lanvid_core_shared PRIVATE "LINKER:-unexported_symbol,_opus_*")
    elseif(UNIX)
      target_link_options(lanvid_core_shared PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    if(WIN32)
      set_target_properties(lanvid_core_shared PROPERTIES ARCHIVE_OUTPUT_NAME "lanvid_core_dll")
    endif()
    target_include_directories(lanvid_core_shared
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/lanvid
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LV_PLATFORM_INCLUDES}
    )
    target_compile_definitions(lanvid_core_shared
      PRIVATE LANVID_BUILD_SHARED ${LV_PRIVATE_DEFS}
      INTERFACE LANVID_USE_SHARED=1
    )
    target_link_libraries(lanvid_core_shared PRIVATE ${LV_PLATFORM_LIBS} $<BUILD_INTERFACE:Opus::opus>)
    lanvid_set_flags(lanvid_core_shared)
  endif()

  # LanVid unit tests
  if(LANVID_BUILD_TESTS AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS
     AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lanvid/test_main.cpp")
    add_executable(lanvid_tests
      tests/lanvid/test_main.cpp
      tests/lanvid/test_dct.cpp
      tests/lanvid/test_huffman.cpp
      tests/lanvid/test_codec_roundtrip.cpp
      tests/lanvid/test_video_codecs.cpp
      tests/lanvid/test_parallel.cpp
      tests/lanvid/test_packet.cpp
      tests/lanvid/test_assembler.cpp
      tests/lanvid/test_fec.cpp
      tests/lanvid/test_audio.cpp
      tests/lanvid/test_multicam_iso_vcam.cpp
      tests/lanvid/test_c_api.cpp
    )
    target_include_directories(lanvid_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
      ${CMAKE_CURRENT_SOURCE_DIR}/src/lanvid
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(lanvid_tests PRIVATE
      NET_COMMON_NS=lv_sys
      NET_COMMON_IMPORT_NS=lv
      LV_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(lanvid_tests PRIVATE lanvid_core)
    lanvid_set_flags(lanvid_tests)
    add_test(NAME lanvid_tests COMMAND lanvid_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()

  # LanVid CMake Install & pkg-config (COMPONENT lanvid)
  if(NOT ANDROID AND NOT IOS)
    set(LV_INSTALL_TARGETS lanvid_core)
    if(TARGET lanvid_core_shared)
      list(APPEND LV_INSTALL_TARGETS lanvid_core_shared)
    endif()

    install(TARGETS ${LV_INSTALL_TARGETS}
      EXPORT LanVidSDKTargets
      COMPONENT lanvid
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    if(_OPUS_FROM_FETCHCONTENT AND TARGET opus)
      install(FILES $<TARGET_FILE:opus>
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT lanvid
      )
    endif()

    install(DIRECTORY sdk/include/lanvid
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
      COMPONENT lanvid
    )
    install(FILES sdk/cpp/lanvid.hpp
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lanvid
      COMPONENT lanvid
    )

    install(EXPORT LanVidSDKTargets
      FILE LanVidSDKTargets.cmake
      NAMESPACE LanVidSDK::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LanVidSDK
      COMPONENT lanvid
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
      COMPONENT lanvid
    )

    configure_file(
      sdk/cmake/lanvid-sdk.pc.in
      "${CMAKE_CURRENT_BINARY_DIR}/lanvid-sdk.pc"
      @ONLY
    )
    install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/lanvid-sdk.pc"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
      COMPONENT lanvid
    )
  endif()
endif() # _ENABLE_LANVID

# ==============================================================================
# PROTOCOL 2: CrossLink Audio / MIDI / Grid SDK (v1.0.0)
# ==============================================================================
if(_ENABLE_CROSSLINK)
  set(CL_CORE_SOURCES
    ${COMMON_NET_SOURCES}
    src/crosslink/net/packet.cpp
    src/crosslink/net/audio_sender.cpp
    src/crosslink/net/audio_receiver.cpp
    src/crosslink/net/audio_discovery.cpp
    src/crosslink/audio/master_clock.cpp
    src/crosslink/audio/audio_sync_matrix.cpp
    src/crosslink/audio/audio_tx.cpp
    src/crosslink/audio/audio_rx.cpp
    src/crosslink/audio/channel_patchbay.cpp
    src/crosslink/audio/multi_device_matrix.cpp
    src/crosslink/audio/personal_monitor_bus.cpp
    src/crosslink/midi/midi_sync_engine.cpp
    src/crosslink/midi/midi_dejitter_queue.cpp
    src/crosslink/midi/midi_device_manager.cpp
    src/crosslink/grid/l2_protocol.cpp
    src/crosslink/grid/l2_transport.cpp
    src/crosslink/grid/grid_node.cpp
    src/crosslink/grid/dsp_server.cpp
    src/crosslink/core/crosslink_c_api.cpp
  )

  set(CL_PRIVATE_DEFS
    NET_COMMON_NS=cl_sys
    NET_COMMON_IMPORT_NS=cl
    NET_SIMD_ENV_VAR="CL_SIMD"
    CROSSLINK_VERSION_MAJOR=${CROSSLINK_SDK_VER_MAJOR}
    CROSSLINK_VERSION_MINOR=${CROSSLINK_SDK_VER_MINOR}
    CROSSLINK_VERSION_PATCH=${CROSSLINK_SDK_VER_PATCH}
    CROSSLINK_VERSION_STRING="${CROSSLINK_SDK_VERSION}"
    CL_HAVE_OPUS=1
  )

  # Desktop-only: CrossLinkAudioBus_v1 shared-memory virtual device bridge
  if(NOT ANDROID AND NOT IOS)
    list(APPEND CL_CORE_SOURCES
      src/crosslink/audio/shm_audio_bridge.cpp
      src/crosslink/audio/shm_audio_host.cpp
    )
    list(APPEND CL_PRIVATE_DEFS CL_HAVE_SHM_BUS=1)
  endif()

  # CrossLink platform audio/MIDI libs (ZERO VideoToolbox / CoreVideo / D3D11 / MediaFoundation / FFmpeg!)
  set(CL_PLATFORM_LIBS Threads::Threads)
  set(CROSSLINK_PKGCONFIG_PRIVATE_LIBS "-lpthread")
  if(COMMON_RT_LIB)
    list(APPEND CL_PLATFORM_LIBS ${COMMON_RT_LIB})
    string(APPEND CROSSLINK_PKGCONFIG_PRIVATE_LIBS " -lrt")
  endif()

  if(WIN32)
    list(APPEND CL_CORE_SOURCES src/crosslink/midi/virtual_midi_win.cpp)
    list(APPEND CL_PLATFORM_LIBS ws2_32 iphlpapi ole32 uuid winmm avrt mmdevapi)
    set(CROSSLINK_PKGCONFIG_PRIVATE_LIBS "-lws2_32 -liphlpapi -lole32 -luuid -lwinmm -lavrt -lmmdevapi")
  elseif(APPLE)
    list(APPEND CL_CORE_SOURCES src/crosslink/midi/virtual_midi_mac.mm)
    list(APPEND CL_PLATFORM_LIBS
      "-framework CoreMIDI"
      "-framework CoreFoundation"
    )
    set(CROSSLINK_PKGCONFIG_PRIVATE_LIBS "-lpthread -framework CoreMIDI -framework CoreFoundation")
    if(NOT IOS)
      list(APPEND CL_PLATFORM_LIBS "-framework CoreAudio" "-framework SystemConfiguration")
      string(APPEND CROSSLINK_PKGCONFIG_PRIVATE_LIBS " -framework CoreAudio -framework SystemConfiguration")
    endif()
  elseif(ANDROID)
    list(APPEND CL_CORE_SOURCES src/crosslink/midi/virtual_midi_stub.cpp)
    list(APPEND CL_PLATFORM_LIBS log)
  else()
    list(APPEND CL_CORE_SOURCES src/crosslink/midi/virtual_midi_stub.cpp)
  endif()

  # Static CrossLink core
  add_library(crosslink_core STATIC ${CL_CORE_SOURCES})
  add_library(CrossLinkSDK::AudioStatic ALIAS crosslink_core)
  set_target_properties(crosslink_core PROPERTIES EXPORT_NAME "AudioStatic")
  target_include_directories(crosslink_core
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/crosslink
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(crosslink_core PRIVATE ${CL_PRIVATE_DEFS})
  target_link_libraries(crosslink_core PUBLIC ${CL_PLATFORM_LIBS} $<BUILD_INTERFACE:Opus::opus>)
  lanvid_set_flags(crosslink_core)

  # Shared CrossLink C ABI SDK library (DLL/.so/.dylib)
  if(LANVID_BUILD_SHARED)
    add_library(crosslink_core_shared SHARED ${CL_CORE_SOURCES})
    add_library(CrossLinkSDK::Audio ALIAS crosslink_core_shared)
    set_target_properties(crosslink_core_shared PROPERTIES
      OUTPUT_NAME "crosslink_core"
      EXPORT_NAME "Audio"
      VERSION ${CROSSLINK_SDK_VERSION}
      SOVERSION ${CROSSLINK_SDK_VER_MAJOR}
      CXX_VISIBILITY_PRESET hidden
      OBJCXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
    )
    if(APPLE)
      target_link_options(crosslink_core_shared PRIVATE "LINKER:-unexported_symbol,_opus_*")
    elseif(UNIX)
      target_link_options(crosslink_core_shared PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    if(WIN32)
      set_target_properties(crosslink_core_shared PROPERTIES ARCHIVE_OUTPUT_NAME "crosslink_core_dll")
    endif()
    target_include_directories(crosslink_core_shared
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk/cpp>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/crosslink
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(crosslink_core_shared
      PRIVATE CROSSLINK_BUILD_SHARED ${CL_PRIVATE_DEFS}
      INTERFACE CROSSLINK_USE_SHARED=1
    )
    target_link_libraries(crosslink_core_shared PRIVATE ${CL_PLATFORM_LIBS} $<BUILD_INTERFACE:Opus::opus>)
    lanvid_set_flags(crosslink_core_shared)
  endif()

  # CrossLink unit tests
  if(LANVID_BUILD_TESTS AND NOT LANVID_SDK_CORE_ONLY AND NOT ANDROID AND NOT IOS
     AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/crosslink/test_main.cpp")
    add_executable(crosslink_tests
      tests/crosslink/test_main.cpp
      tests/crosslink/test_packet_crosslink.cpp
      tests/crosslink/test_audio_64ch.cpp
      tests/crosslink/test_audio_duplex.cpp
      tests/crosslink/test_audio_restart_safety.cpp
      tests/crosslink/test_latency_mode.cpp
      tests/crosslink/test_rate_integrity.cpp
      tests/crosslink/test_shm_audio_bus.cpp
      tests/crosslink/test_multi_device_matrix.cpp
      tests/crosslink/test_coreaudio_matrix.cpp
      tests/crosslink/test_coremidi_ports.cpp
      tests/crosslink/test_midi_sync.cpp
      tests/crosslink/test_udp_e2e_audio_midi.cpp
      tests/crosslink/test_network_interfaces.cpp
      tests/crosslink/test_crosslink_grid_l2.cpp
      tests/crosslink/test_crosslink_c_api.cpp
    )
    target_include_directories(crosslink_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
      ${CMAKE_CURRENT_SOURCE_DIR}/src/crosslink
      ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(crosslink_tests PRIVATE
      NET_COMMON_NS=cl_sys
      NET_COMMON_IMPORT_NS=cl
      CL_HAVE_SHM_BUS=1
      CL_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(crosslink_tests PRIVATE crosslink_core)
    lanvid_set_flags(crosslink_tests)
    add_test(NAME crosslink_tests COMMAND crosslink_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()

  # CrossLink CMake Install & pkg-config (COMPONENT crosslink)
  if(NOT ANDROID AND NOT IOS)
    set(CL_INSTALL_TARGETS crosslink_core)
    if(TARGET crosslink_core_shared)
      list(APPEND CL_INSTALL_TARGETS crosslink_core_shared)
    endif()

    install(TARGETS ${CL_INSTALL_TARGETS}
      EXPORT CrossLinkSDKTargets
      COMPONENT crosslink
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    if(_OPUS_FROM_FETCHCONTENT AND TARGET opus)
      install(FILES $<TARGET_FILE:opus>
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT crosslink
      )
    endif()

    install(DIRECTORY sdk/include/crosslink
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
      COMPONENT crosslink
    )
    install(FILES sdk/cpp/crosslink.hpp
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/crosslink
      COMPONENT crosslink
    )

    install(EXPORT CrossLinkSDKTargets
      FILE CrossLinkSDKTargets.cmake
      NAMESPACE CrossLinkSDK::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossLinkSDK
      COMPONENT crosslink
    )

    configure_package_config_file(
      sdk/cmake/CrossLinkSDKConfig.cmake.in
      "${CMAKE_CURRENT_BINARY_DIR}/CrossLinkSDKConfig.cmake"
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossLinkSDK
    )
    write_basic_package_version_file(
      "${CMAKE_CURRENT_BINARY_DIR}/CrossLinkSDKConfigVersion.cmake"
      VERSION ${CROSSLINK_SDK_VERSION}
      COMPATIBILITY SameMajorVersion
    )
    install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/CrossLinkSDKConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/CrossLinkSDKConfigVersion.cmake"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossLinkSDK
      COMPONENT crosslink
    )

    configure_file(
      sdk/cmake/crosslink-sdk.pc.in
      "${CMAKE_CURRENT_BINARY_DIR}/crosslink-sdk.pc"
      @ONLY
    )
    install(FILES
      "${CMAKE_CURRENT_BINARY_DIR}/crosslink-sdk.pc"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
      COMPONENT crosslink
    )
  endif()
endif() # _ENABLE_CROSSLINK
