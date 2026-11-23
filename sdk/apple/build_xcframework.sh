#!/usr/bin/env bash
# Builds universal Apple XCFramework: dist/apple/LanVidCore.xcframework
# Slices:
#   1. ios-arm64                    (Physical iPhone / iPad, ARM64 NEON enabled)
#   2. ios-arm64_x86_64-simulator   (iOS Simulator: Apple Silicon + Intel)
#   3. macos-arm64_x86_64           (Universal Binary: Apple Silicon M1-M4 + Intel Mac)
#
# Run on macOS with Xcode Command Line Tools + CMake installed:
#   ./sdk/apple/build_xcframework.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPLE_DIR="$ROOT/sdk/apple"
BUILD_ROOT="$ROOT/build-apple"
DIST_APPLE="$ROOT/dist/apple"
STAGE_HEADERS="$BUILD_ROOT/headers"

rm -rf "$BUILD_ROOT" "$DIST_APPLE/LanVidCore.xcframework"
mkdir -p "$BUILD_ROOT" "$DIST_APPLE" "$STAGE_HEADERS/lanvid"

# Stage self-contained public SDK headers + Swift module.modulemap
cp "$ROOT/sdk/include/lanvid/"*.h "$STAGE_HEADERS/lanvid/"
cp "$APPLE_DIR/installer/module.modulemap" "$STAGE_HEADERS/module.modulemap"

build_slice() {
  local name="$1"
  local sysname="$2"
  local sysroot="$3"
  local archs="$4"
  local deploy_flag="$5"
  local neon_flag="$6"
  local avx2_flag="${7:-OFF}"

  local bdir="$BUILD_ROOT/$name"
  local gen="Unix Makefiles"
  if command -v ninja >/dev/null 2>&1; then gen="Ninja"; fi
  echo "=== Building Apple slice: $name ($archs, NEON=$neon_flag, AVX2=$avx2_flag) ==="
  cmake -S "$ROOT" -B "$bdir" -G "$gen" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME="$sysname" \
    -DCMAKE_OSX_SYSROOT="$sysroot" \
    -DCMAKE_OSX_ARCHITECTURES="$archs" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploy_flag" \
    -DLANVID_SDK_CORE_ONLY=ON \
    -DLANVID_BUILD_SHARED=OFF \
    -DLANVID_BUILD_GUI=OFF \
    -DLANVID_BUILD_CLI=OFF \
    -DLANVID_BUILD_TESTS=OFF \
    -DLANVID_ENABLE_AVX2="$avx2_flag" \
    -DLANVID_ENABLE_NEON="$neon_flag"
  cmake --build "$bdir" --target lanvid_core --parallel "$(sysctl -n hw.ncpu)"

  # liblanvid_core.a only references libopus (AUDIO2 Opus codec): fold it in so the slice is self-contained.
  local opus_lib
  opus_lib="$(find "$bdir" -name 'libopus.a' | head -n 1)"
  if [ -z "$opus_lib" ]; then echo "libopus.a not found in $bdir" >&2; exit 1; fi
  libtool -static -o "$bdir/liblanvid_core_opus.a" "$bdir/liblanvid_core.a" "$opus_lib"
  mv "$bdir/liblanvid_core_opus.a" "$bdir/liblanvid_core.a"
}

# 1. iOS Device (arm64 + NEON)
build_slice "ios-arm64" "iOS" "iphoneos" "arm64" "15.0" "ON" "OFF"

# 2. iOS Simulator (arm64 + x86_64 fat static lib via lipo)
build_slice "ios-sim-arm64"  "iOS" "iphonesimulator" "arm64"  "15.0" "ON"  "OFF"
build_slice "ios-sim-x86_64" "iOS" "iphonesimulator" "x86_64" "15.0" "OFF" "OFF"
mkdir -p "$BUILD_ROOT/ios-sim-universal"
lipo -create \
  "$BUILD_ROOT/ios-sim-arm64/liblanvid_core.a" \
  "$BUILD_ROOT/ios-sim-x86_64/liblanvid_core.a" \
  -output "$BUILD_ROOT/ios-sim-universal/liblanvid_core.a"

# 3. macOS Universal Binary (arm64 Apple Silicon NEON + x86_64 Intel AVX2)
build_slice "macos-arm64"  "Darwin" "macosx" "arm64"  "12.0" "ON"  "OFF"
build_slice "macos-x86_64" "Darwin" "macosx" "x86_64" "12.0" "OFF" "ON"
mkdir -p "$BUILD_ROOT/macos-universal"
lipo -create \
  "$BUILD_ROOT/macos-arm64/liblanvid_core.a" \
  "$BUILD_ROOT/macos-x86_64/liblanvid_core.a" \
  -output "$BUILD_ROOT/macos-universal/liblanvid_core.a"

# 4. Assemble universal XCFramework with module.modulemap
xcodebuild -create-xcframework \
  -library "$BUILD_ROOT/ios-arm64/liblanvid_core.a" -headers "$STAGE_HEADERS" \
  -library "$BUILD_ROOT/ios-sim-universal/liblanvid_core.a" -headers "$STAGE_HEADERS" \
  -library "$BUILD_ROOT/macos-universal/liblanvid_core.a" -headers "$STAGE_HEADERS" \
  -output "$DIST_APPLE/LanVidCore.xcframework"

echo "Successfully created: $DIST_APPLE/LanVidCore.xcframework"
