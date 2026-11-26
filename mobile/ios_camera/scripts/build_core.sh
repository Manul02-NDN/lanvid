#!/usr/bin/env bash
# Builds mobile/ios_camera/build/LanVidCore.xcframework (iPhone arm64 + Simulator arm64/x86_64)
# from the LanVid core (CMake, LANVID_SDK_CORE_ONLY + libopus + TallyIntercom + VideoToolbox).
#   ./mobile/ios_camera/scripts/build_core.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
HDR="$OUT/headers"
DEPLOY="16.0"
GEN="Unix Makefiles"; command -v ninja >/dev/null 2>&1 && GEN="Ninja"

# Reuse fresh sibling xcframework if explicitly requested (local fast iteration)
if [ "${LANVID_REUSE_MONITOR_CORE:-0}" = "1" ] && [ -d "$ROOT/mobile/ios/build/LanVidCore.xcframework" ]; then
  mkdir -p "$OUT"
  rm -rf "$OUT/LanVidCore.xcframework"
  cp -R "$ROOT/mobile/ios/build/LanVidCore.xcframework" "$OUT/LanVidCore.xcframework"
  echo "Copied $OUT/LanVidCore.xcframework from mobile/ios/build"
  exit 0
fi

rm -rf "$OUT/LanVidCore.xcframework" "$HDR"
mkdir -p "$HDR/lanvid"
cp "$ROOT/sdk/include/lanvid/"*.h "$HDR/lanvid/"
cp "$ROOT/sdk/apple/installer/module.modulemap" "$HDR/module.modulemap"

slice() {  # name sysroot archs neon
  local b="$OUT/core-$1"
  cmake -S "$ROOT" -B "$b" -G "$GEN" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$2" \
    -DCMAKE_OSX_ARCHITECTURES="$3" -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
    -DLANVID_SDK_CORE_ONLY=ON -DLANVID_BUILD_SHARED=OFF -DLANVID_BUILD_GUI=OFF -DLANVID_BUILD_CLI=OFF \
    -DLANVID_BUILD_TESTS=OFF -DLANVID_ENABLE_AVX2=OFF -DLANVID_ENABLE_NEON="$4" >/dev/null
  cmake --build "$b" --target lanvid_core --parallel "$(sysctl -n hw.ncpu)"
  merge_opus "$b"
}

merge_opus() {
  local b="$1" opus
  opus="$(find "$b" -name 'libopus.a' | head -n 1)"
  [ -n "$opus" ] || { echo "libopus.a not found in $b" >&2; exit 1; }
  libtool -static -o "$b/liblanvid_core_opus.a" "$b/liblanvid_core.a" "$opus"
  mv "$b/liblanvid_core_opus.a" "$b/liblanvid_core.a"
}

slice ios-arm64 iphoneos arm64 ON
slice sim-arm64 iphonesimulator arm64 ON
slice sim-x86_64 iphonesimulator x86_64 OFF
mkdir -p "$OUT/core-sim-universal"
lipo -create "$OUT/core-sim-arm64/liblanvid_core.a" "$OUT/core-sim-x86_64/liblanvid_core.a" \
     -output "$OUT/core-sim-universal/liblanvid_core.a"

xcodebuild -create-xcframework \
  -library "$OUT/core-ios-arm64/liblanvid_core.a" -headers "$HDR" \
  -library "$OUT/core-sim-universal/liblanvid_core.a" -headers "$HDR" \
  -output "$OUT/LanVidCore.xcframework"
echo "Built $OUT/LanVidCore.xcframework"
