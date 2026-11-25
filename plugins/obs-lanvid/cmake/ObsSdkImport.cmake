# Imports OBS::libobs and OBS::obs-frontend-api from a "light" OBS SDK when the official CMake packages
# (find_package(libobs) / find_package(obs-frontend-api), i.e. a full OBS source build) are not available.
#
#   LANVID_OBS_SDK_DIR/include   libobs headers + obs-frontend-api.h + obsconfig.h
#                                (prepared by tools/fetch_obs_sdk.sh / fetch_obs_sdk.ps1 for the installed OBS)
#   Windows: LANVID_OBS_SDK_DIR/lib/obs.lib + obs-frontend-api.lib   (import libraries of the installed DLLs)
#   macOS:   LANVID_OBS_APP (default /Applications/OBS.app) -> libobs.framework + obs-frontend-api.dylib
#
# The plugin links against the exact binaries OBS loads it into; nothing is redistributed.
if(TARGET OBS::libobs AND TARGET OBS::obs-frontend-api)
  return()
endif()
set(LANVID_OBS_SDK_DIR "" CACHE PATH "Light OBS SDK (headers + import libs) for building obs-lanvid without an OBS source build")
if(NOT LANVID_OBS_SDK_DIR OR NOT EXISTS "${LANVID_OBS_SDK_DIR}/include/obs-module.h")
  return()
endif()

set(_inc "${LANVID_OBS_SDK_DIR}/include")
if(WIN32)
  set(_libobs "${LANVID_OBS_SDK_DIR}/lib/obs.lib")
  set(_fe "${LANVID_OBS_SDK_DIR}/lib/obs-frontend-api.lib")
  if(NOT EXISTS "${_libobs}" OR NOT EXISTS "${_fe}")
    message(STATUS "obs-lanvid: ${LANVID_OBS_SDK_DIR}/lib has no obs.lib / obs-frontend-api.lib")
    return()
  endif()
  add_library(OBS::libobs UNKNOWN IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${_libobs}" INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
  add_library(OBS::obs-frontend-api UNKNOWN IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${_fe}" INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
elseif(APPLE)
  set(LANVID_OBS_APP "/Applications/OBS.app" CACHE PATH "Installed OBS Studio app bundle to link obs-lanvid against")
  set(_fw "${LANVID_OBS_APP}/Contents/Frameworks")
  if(NOT EXISTS "${_fw}/libobs.framework" OR NOT EXISTS "${_fw}/obs-frontend-api.dylib")
    message(STATUS "obs-lanvid: ${LANVID_OBS_APP} has no libobs.framework / obs-frontend-api.dylib")
    return()
  endif()
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "${_fw}/libobs.framework/Versions/A/libobs"
    IMPORTED_SONAME "@rpath/libobs.framework/Versions/A/libobs"
    INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
  add_library(OBS::obs-frontend-api SHARED IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES
    IMPORTED_LOCATION "${_fw}/obs-frontend-api.dylib"
    IMPORTED_SONAME "@rpath/obs-frontend-api.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
else()
  return()
endif()
set(libobs_FOUND TRUE)
set(obs-frontend-api_FOUND TRUE)
message(STATUS "obs-lanvid: using light OBS SDK at ${LANVID_OBS_SDK_DIR}")
