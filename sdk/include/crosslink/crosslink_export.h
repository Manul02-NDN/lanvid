#pragma once
/*
 * CrossLink Audio SDK — Export Macros & ABI Version Control (v1.0.0)
 * Cross-platform C99/C11 public ABI for 64-channel AoIP, MIDI2, Personal Monitor Bus,
 * Shared-Memory Audio Bus, and Layer 2 Grid audio transport.
 */

#include <stdint.h>

#define CL_SDK_VERSION_MAJOR 1
#define CL_SDK_VERSION_MINOR 0
#define CL_SDK_VERSION_PATCH 0
#define CL_SDK_VERSION_STRING "1.0.0"
#define CL_SDK_ABI_VERSION   1

#if defined(_WIN32)
#  if defined(CROSSLINK_BUILD_SHARED)
#    define CL_API __declspec(dllexport)
#  elif defined(CROSSLINK_USE_SHARED)
#    define CL_API __declspec(dllimport)
#  else
#    define CL_API
#  endif
#else
#  if defined(CROSSLINK_BUILD_SHARED) || defined(__GNUC__) || defined(__clang__)
#    define CL_API __attribute__((visibility("default")))
#  else
#    define CL_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns semantic SDK version string (e.g. "1.0.0"). */
CL_API const char* cl_get_version_string(void);

/* Returns packed version integer: (MAJOR << 16) | (MINOR << 8) | PATCH. */
CL_API uint32_t cl_get_version_number(void);

/* Verifies binary ABI compatibility against caller's compile-time header version.
 * Returns 1 if compatible, 0 if incompatible. */
CL_API int cl_check_abi_compatibility(int header_major, int header_minor);

#ifdef __cplusplus
}
#endif
