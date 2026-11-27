#pragma once
/*
 * LanVid SDK — Export Macros & ABI Version Control
 * Cross-platform C99/C11 public ABI for Windows, Linux, macOS, iOS, and Android.
 */

#include <stdint.h>

#define LV_SDK_VERSION_MAJOR 2
#define LV_SDK_VERSION_MINOR 7
#define LV_SDK_VERSION_PATCH 0
#define LV_SDK_VERSION_STRING "2.7.0"
#define LV_SDK_ABI_VERSION   2

#if defined(_WIN32)
#  if defined(LANVID_BUILD_SHARED)
#    define LV_API __declspec(dllexport)
#  elif defined(LANVID_USE_SHARED)
#    define LV_API __declspec(dllimport)
#  else
#    define LV_API
#  endif
#else
#  if defined(LANVID_BUILD_SHARED) || defined(__GNUC__) || defined(__clang__)
#    define LV_API __attribute__((visibility("default")))
#  else
#    define LV_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns semantic SDK version string (e.g. "1.0.0"). */
LV_API const char* lv_get_version_string(void);

/* Returns packed version integer: (MAJOR << 16) | (MINOR << 8) | PATCH. */
LV_API uint32_t lv_get_version_number(void);

/* Verifies binary ABI compatibility against caller's compile-time header version.
 * Returns 1 if compatible, 0 if incompatible (major mismatch or runtime older than header). */
LV_API int lv_check_abi_compatibility(int header_major, int header_minor);

#ifdef __cplusplus
}
#endif
