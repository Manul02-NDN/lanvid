#!/usr/bin/env bash
# Builds mobile/ios_camera/build/LanVidCore.xcframework (iPhone arm64 + Simulator arm64/x86_64)
# from the LanVid Video core (CMake, LANVID_SDK_COMPONENTS=VIDEO + libopus Intercom + VideoToolbox).
#   ./mobile/ios_camera/scripts/build_core.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
DEPLOY="16.0"
GEN="Unix Makefiles"; command -v ninja >/dev/null 2>&1 && GEN="Ninja"

rm -rf "$OUT/LanVidCore.xcframework" "$OUT/fw-ios" "$OUT/fw-sim"
mkdir -p "$OUT"

slice() {  # name sysroot archs neon
  local b="$OUT/core-$1"
  cmake -S "$ROOT" -B "$b" -G "$GEN" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$2" \
    -DCMAKE_OSX_ARCHITECTURES="$3" -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
    -DLANVID_SDK_COMPONENTS=VIDEO -DLANVID_SDK_CORE_ONLY=ON -DLANVID_BUILD_SHARED=OFF \
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

make_ios_fw() {
  local fw="$1" lib_a="$2"
  rm -rf "$fw"
  mkdir -p "$fw/Headers" "$fw/Modules"
  cp "$lib_a" "$fw/LanVidCore"
  cp "$ROOT/sdk/include/lanvid/"*.h "$fw/Headers/"
  cp "$ROOT/sdk/apple/installer/module.modulemap" "$fw/Modules/module.modulemap"
  cat > "$fw/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>LanVidCore</string>
  <key>CFBundleIdentifier</key><string>com.lanvid.LanVidCore</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>LanVidCore</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>3.0.0</string>
  <key>CFBundleVersion</key><string>3.0.0</string>
</dict>
</plist>
PLIST
}

slice ios-arm64 iphoneos arm64 ON
slice sim-arm64 iphonesimulator arm64 ON
slice sim-x86_64 iphonesimulator x86_64 OFF
mkdir -p "$OUT/core-sim-universal"
lipo -create "$OUT/core-sim-arm64/liblanvid_core.a" "$OUT/core-sim-x86_64/liblanvid_core.a" \
     -output "$OUT/core-sim-universal/liblanvid_core.a"

make_ios_fw "$OUT/fw-ios/LanVidCore.framework" "$OUT/core-ios-arm64/liblanvid_core.a"
make_ios_fw "$OUT/fw-sim/LanVidCore.framework" "$OUT/core-sim-universal/liblanvid_core.a"

xcodebuild -create-xcframework \
  -framework "$OUT/fw-ios/LanVidCore.framework" \
  -framework "$OUT/fw-sim/LanVidCore.framework" \
  -output "$OUT/LanVidCore.xcframework"
echo "Built $OUT/LanVidCore.xcframework"
