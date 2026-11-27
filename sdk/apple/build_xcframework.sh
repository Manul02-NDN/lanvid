#!/usr/bin/env bash
# Builds universal Apple XCFrameworks (packaged as static .framework bundles so both
# LanVidCore.xcframework and CrossLinkCore.xcframework coexist in one SwiftPM / Xcode app
# with zero module.modulemap collisions):
#   - dist/apple/LanVidCore.xcframework    (Video + 2-ch Stereo Audio, v3.0.0)
#   - dist/apple/CrossLinkCore.xcframework (64-ch AoIP + MIDI 2.0 + Monitor, v1.0.0)
#
# Usage:
#   ./sdk/apple/build_xcframework.sh [--protocol all|video|audio]

set -euo pipefail

PROTOCOL="all"
while [ $# -gt 0 ]; do
  case "$1" in
    --protocol) PROTOCOL="${2:-all}"; shift 2 ;;
    --protocol=*) PROTOCOL="${1#*=}"; shift ;;
    *) echo "Usage: $0 [--protocol all|video|audio]" >&2; exit 2 ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPLE_DIR="$ROOT/sdk/apple"
BUILD_ROOT="$ROOT/build-apple"
DIST_APPLE="$ROOT/dist/apple"

mkdir -p "$BUILD_ROOT" "$DIST_APPLE"

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
GEN="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then GEN="Ninja"; fi

make_static_framework() {
  local fw_dir="$1"      # e.g. .../LanVidCore.framework
  local name="$2"        # LanVidCore | CrossLinkCore
  local bundle_id="$3"   # com.lanvid.LanVidCore
  local ver="$4"         # 3.0.0 | 1.0.0
  local lib_a="$5"       # path to static .a
  local hdr_dir="$6"     # sdk/include/<subdir>
  local modmap_file="$7" # module.modulemap path
  local is_macos="$8"    # 1 for macOS versioned bundle, 0 for iOS flat bundle

  rm -rf "$fw_dir"
  local plist_xml
  plist_xml="<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>${name}</string>
  <key>CFBundleIdentifier</key><string>${bundle_id}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>${name}</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>${ver}</string>
  <key>CFBundleVersion</key><string>${ver}</string>
</dict>
</plist>"

  if [ "$is_macos" = "1" ]; then
    mkdir -p "$fw_dir/Versions/A/Headers" "$fw_dir/Versions/A/Modules" "$fw_dir/Versions/A/Resources"
    cp "$lib_a" "$fw_dir/Versions/A/$name"
    cp "$hdr_dir/"*.h "$fw_dir/Versions/A/Headers/"
    cp "$modmap_file" "$fw_dir/Versions/A/Modules/module.modulemap"
    printf '%s\n' "$plist_xml" > "$fw_dir/Versions/A/Resources/Info.plist"
    ln -sfn "A" "$fw_dir/Versions/Current"
    ln -sfn "Versions/Current/$name" "$fw_dir/$name"
    ln -sfn "Versions/Current/Headers" "$fw_dir/Headers"
    ln -sfn "Versions/Current/Modules" "$fw_dir/Modules"
    ln -sfn "Versions/Current/Resources" "$fw_dir/Resources"
  else
    mkdir -p "$fw_dir/Headers" "$fw_dir/Modules"
    cp "$lib_a" "$fw_dir/$name"
    cp "$hdr_dir/"*.h "$fw_dir/Headers/"
    cp "$modmap_file" "$fw_dir/Modules/module.modulemap"
    printf '%s\n' "$plist_xml" > "$fw_dir/Info.plist"
  fi
}

build_protocol_xcframework() {
  local proto_key="$1"    # VIDEO | AUDIO
  local target_name="$2"  # lanvid_core | crosslink_core
  local xcf_name="$3"     # LanVidCore | CrossLinkCore
  local bundle_id="$4"    # com.lanvid.LanVidCore | com.crosslink.CrossLinkCore
  local ver="$5"          # 3.0.0 | 1.0.0
  local hdr_subdir="$6"   # lanvid | crosslink
  local modmap="$7"       # module.modulemap | crosslink_module.modulemap

  local pbuild="$BUILD_ROOT/$target_name"
  rm -rf "$pbuild" "$DIST_APPLE/${xcf_name}.xcframework"
  mkdir -p "$pbuild"

  build_slice() {
    local name="$1"
    local sysname="$2"
    local sysroot="$3"
    local archs="$4"
    local deploy_flag="$5"
    local neon_flag="$6"
    local avx2_flag="${7:-OFF}"

    local bdir="$pbuild/$name"
    echo "=== Building $xcf_name slice: $name ($archs, NEON=$neon_flag, AVX2=$avx2_flag) ==="
    cmake -S "$ROOT" -B "$bdir" -G "$GEN" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_NAME="$sysname" \
      -DCMAKE_OSX_SYSROOT="$sysroot" \
      -DCMAKE_OSX_ARCHITECTURES="$archs" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$deploy_flag" \
      -DLANVID_SDK_COMPONENTS="$proto_key" \
      -DLANVID_SDK_CORE_ONLY=ON \
      -DLANVID_BUILD_SHARED=OFF \
      -DLANVID_BUILD_TESTS=OFF \
      -DLANVID_ENABLE_AVX2="$avx2_flag" \
      -DLANVID_ENABLE_NEON="$neon_flag"
    cmake --build "$bdir" --target "$target_name" --parallel "$NCPU"

    local opus_lib
    opus_lib="$(find "$bdir" -name 'libopus.a' | head -n 1 || true)"
    if [ -n "$opus_lib" ]; then
      libtool -static -o "$bdir/lib${target_name}_merged.a" "$bdir/lib${target_name}.a" "$opus_lib"
      mv "$bdir/lib${target_name}_merged.a" "$bdir/lib${target_name}.a"
    fi
  }

  # 1. iOS Device (arm64 + NEON)
  build_slice "ios-arm64" "iOS" "iphoneos" "arm64" "15.0" "ON" "OFF"

  # 2. iOS Simulator (arm64 + x86_64 fat static lib)
  build_slice "ios-sim-arm64"  "iOS" "iphonesimulator" "arm64"  "15.0" "ON"  "OFF"
  build_slice "ios-sim-x86_64" "iOS" "iphonesimulator" "x86_64" "15.0" "OFF" "OFF"
  mkdir -p "$pbuild/ios-sim-universal"
  lipo -create \
    "$pbuild/ios-sim-arm64/lib${target_name}.a" \
    "$pbuild/ios-sim-x86_64/lib${target_name}.a" \
    -output "$pbuild/ios-sim-universal/lib${target_name}.a"

  # 3. macOS Universal Binary (arm64 + x86_64)
  build_slice "macos-arm64"  "Darwin" "macosx" "arm64"  "12.0" "ON"  "OFF"
  build_slice "macos-x86_64" "Darwin" "macosx" "x86_64" "12.0" "OFF" "ON"
  mkdir -p "$pbuild/macos-universal"
  lipo -create \
    "$pbuild/macos-arm64/lib${target_name}.a" \
    "$pbuild/macos-x86_64/lib${target_name}.a" \
    -output "$pbuild/macos-universal/lib${target_name}.a"

  # 4. Wrap each slice in a static .framework bundle & assemble XCFramework
  local hdr_src="$ROOT/sdk/include/$hdr_subdir"
  local mm_src="$APPLE_DIR/installer/$modmap"
  make_static_framework "$pbuild/fw-ios/${xcf_name}.framework"     "$xcf_name" "$bundle_id" "$ver" "$pbuild/ios-arm64/lib${target_name}.a"         "$hdr_src" "$mm_src" 0
  make_static_framework "$pbuild/fw-ios-sim/${xcf_name}.framework" "$xcf_name" "$bundle_id" "$ver" "$pbuild/ios-sim-universal/lib${target_name}.a" "$hdr_src" "$mm_src" 0
  make_static_framework "$pbuild/fw-macos/${xcf_name}.framework"   "$xcf_name" "$bundle_id" "$ver" "$pbuild/macos-universal/lib${target_name}.a"   "$hdr_src" "$mm_src" 1

  xcodebuild -create-xcframework \
    -framework "$pbuild/fw-ios/${xcf_name}.framework" \
    -framework "$pbuild/fw-ios-sim/${xcf_name}.framework" \
    -framework "$pbuild/fw-macos/${xcf_name}.framework" \
    -output "$DIST_APPLE/${xcf_name}.xcframework"

  echo "[OK] Created: $DIST_APPLE/${xcf_name}.xcframework"
}

case "$PROTOCOL" in
  all)
    build_protocol_xcframework "VIDEO" "lanvid_core"    "LanVidCore"    "com.lanvid.LanVidCore"       "3.0.0" "lanvid"    "module.modulemap"
    build_protocol_xcframework "AUDIO" "crosslink_core" "CrossLinkCore" "com.crosslink.CrossLinkCore" "1.0.0" "crosslink" "crosslink_module.modulemap"
    ;;
  video)
    build_protocol_xcframework "VIDEO" "lanvid_core"    "LanVidCore"    "com.lanvid.LanVidCore"       "3.0.0" "lanvid"    "module.modulemap"
    ;;
  audio)
    build_protocol_xcframework "AUDIO" "crosslink_core" "CrossLinkCore" "com.crosslink.CrossLinkCore" "1.0.0" "crosslink" "crosslink_module.modulemap"
    ;;
  *)
    echo "[ERROR] Invalid --protocol '$PROTOCOL'. Expected: all, video, audio." >&2
    exit 2
    ;;
esac
