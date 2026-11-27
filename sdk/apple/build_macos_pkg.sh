#!/usr/bin/env bash
# LanVid Video SDK (v3.1.0) & CrossLink Audio SDK (v1.0.0) macOS Universal Installer Builder
# Produces a single macOS .pkg & .dmg where the user can interactively choose:
#   - Install Both (LanVid Video + CrossLink Audio) [default]
#   - Install LanVid Video SDK only (Video + 2-ch Stereo Audio)
#   - Install CrossLink Audio SDK only (64-ch AoIP + MIDI 2.0 + Personal Monitor + L2 Grid)
#
# Usage:
#   ./sdk/apple/build_macos_pkg.sh [--skip-xcframework]

set -euo pipefail

LANVID_VERSION="3.1.0"
CROSSLINK_VERSION="1.0.0"
BUNDLE_VERSION="3.1.0"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPLE_DIR="$ROOT/sdk/apple"
INST_DIR="$APPLE_DIR/installer"
BUILD_DIR="$ROOT/build-macos-pkg"
DIST_APPLE="$ROOT/dist/apple"
PKG_OUT="$DIST_APPLE/LanVidCrossLinkSDK-v${BUNDLE_VERSION}-macOS.pkg"
DMG_OUT="$DIST_APPLE/LanVidCrossLinkSDK-v${BUNDLE_VERSION}-macOS.dmg"

for arg in "$@"; do
  case "$arg" in
    --skip-xcframework) LANVID_SKIP_XCFRAMEWORK=1 ;;
  esac
done

if [ "$(uname -s)" != "Darwin" ]; then
  echo "[ERROR] build_macos_pkg.sh must be executed on macOS." >&2
  exit 1
fi

for req in cmake lipo install_name_tool pkgbuild productbuild hdiutil; do
  if ! command -v "$req" >/dev/null 2>&1; then
    echo "[ERROR] Required macOS tool '$req' not found." >&2
    exit 1
  fi
done

NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then GENERATOR="Ninja"; fi

echo "=== [1/6] Cleaning staging directories ==="
rm -rf "$BUILD_DIR" "$PKG_OUT" "$DMG_OUT"
mkdir -p "$BUILD_DIR" "$DIST_APPLE"

build_macos_arch() {
  local arch="$1"
  local neon="$2"
  local avx2="$3"
  local bdir="$BUILD_DIR/macos-$arch"
  local idir="$BUILD_DIR/install-$arch"

  echo "=== Building Universal slice $arch (LanVid + CrossLink, NEON=$neon, AVX2=$avx2) ==="
  cmake -S "$ROOT" -B "$bdir" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DLANVID_SDK_COMPONENTS=ALL \
    -DLANVID_SDK_CORE_ONLY=ON \
    -DLANVID_BUILD_SHARED=ON \
    -DLANVID_BUILD_TESTS=OFF \
    -DLANVID_ENABLE_NEON="$neon" \
    -DLANVID_ENABLE_AVX2="$avx2"
  cmake --build "$bdir" --target lanvid_core lanvid_core_shared crosslink_core crosslink_core_shared --parallel "$NCPU"
  cmake --install "$bdir" --prefix "$idir"
}

echo "=== [2/6] Compiling Universal macOS SDK libraries (arm64 NEON + x86_64 AVX2) ==="
build_macos_arch "arm64"  "ON"  "OFF"
build_macos_arch "x86_64" "OFF" "ON"

UNIV_LIB_DIR="$BUILD_DIR/universal-lib"
mkdir -p "$UNIV_LIB_DIR"

make_universal() {
  local libname="$1"
  local install_id="$2"
  lipo -create \
    "$BUILD_DIR/install-arm64/lib/${libname}.dylib" \
    "$BUILD_DIR/install-x86_64/lib/${libname}.dylib" \
    -output "$UNIV_LIB_DIR/${libname}.dylib"
  lipo -create \
    "$BUILD_DIR/install-arm64/lib/${libname}.a" \
    "$BUILD_DIR/install-x86_64/lib/${libname}.a" \
    -output "$UNIV_LIB_DIR/${libname}.a"
  install_name_tool -id "$install_id" "$UNIV_LIB_DIR/${libname}.dylib"
  if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
    codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$UNIV_LIB_DIR/${libname}.dylib"
  else
    codesign --force --sign - "$UNIV_LIB_DIR/${libname}.dylib"
  fi
  echo "[OK] Universal ${libname}: $(lipo -archs "$UNIV_LIB_DIR/${libname}.dylib")"
}

make_universal "liblanvid_core"    "/usr/local/lib/liblanvid_core.dylib"
make_universal "libcrosslink_core" "/usr/local/lib/libcrosslink_core.dylib"

# Universal libopus.a (shared static dependency for static CMake consumers, zero conflict when both installed)
if [ -f "$BUILD_DIR/install-arm64/lib/libopus.a" ] && [ -f "$BUILD_DIR/install-x86_64/lib/libopus.a" ]; then
  lipo -create \
    "$BUILD_DIR/install-arm64/lib/libopus.a" \
    "$BUILD_DIR/install-x86_64/lib/libopus.a" \
    -output "$UNIV_LIB_DIR/libopus.a"
fi

echo "=== [3/6] Building XCFrameworks (LanVidCore + CrossLinkCore) ==="
if [ -z "${LANVID_SKIP_XCFRAMEWORK:-}" ]; then
  bash "$APPLE_DIR/build_xcframework.sh" --protocol all
fi

echo "=== [4/6] Staging protocol-isolated installer component roots ==="

# --- Component 1: LanVid Video SDK (v3.0.0) ---
ROOT_LV="$BUILD_DIR/pkgroot-lanvid"
LV_STAGE="$ROOT_LV/usr/local/LanVidSDK"
mkdir -p "$LV_STAGE/include/lanvid" "$LV_STAGE/lib/cmake/LanVidSDK" "$LV_STAGE/lib/pkgconfig" \
         "$LV_STAGE/apple" "$LV_STAGE/python" "$LV_STAGE/examples" "$ROOT_LV/usr/local/lib"

cp "$BUILD_DIR/install-arm64/include/lanvid/"* "$LV_STAGE/include/lanvid/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$LV_STAGE/lib/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$ROOT_LV/usr/local/lib/liblanvid_core.dylib"
cp "$UNIV_LIB_DIR/liblanvid_core.a"     "$LV_STAGE/lib/"
[ -f "$UNIV_LIB_DIR/libopus.a" ] && cp "$UNIV_LIB_DIR/libopus.a" "$LV_STAGE/lib/"
cp "$BUILD_DIR/install-arm64/lib/cmake/LanVidSDK/"* "$LV_STAGE/lib/cmake/LanVidSDK/"
sed "s|^prefix=.*|prefix=/usr/local/LanVidSDK|" \
  "$BUILD_DIR/install-arm64/lib/pkgconfig/lanvid-sdk.pc" > "$LV_STAGE/lib/pkgconfig/lanvid-sdk.pc"
[ -d "$DIST_APPLE/LanVidCore.xcframework" ] && cp -R "$DIST_APPLE/LanVidCore.xcframework" "$LV_STAGE/apple/"
cp -R "$APPLE_DIR/LanVidKit" "$LV_STAGE/apple/"
cp "$ROOT/sdk/python/lanvid_sdk.py" "$LV_STAGE/python/"
[ -d "$ROOT/sdk/examples/cpp_video_client" ] && cp -R "$ROOT/sdk/examples/cpp_video_client" "$LV_STAGE/examples/"
cp -R "$APPLE_DIR/Examples" "$LV_STAGE/apple-examples"
cp "$ROOT/sdk/README.md" "$LV_STAGE/README.md"

# --- Component 2: CrossLink Audio SDK (v1.0.0) ---
ROOT_CL="$BUILD_DIR/pkgroot-crosslink"
CL_STAGE="$ROOT_CL/usr/local/CrossLinkSDK"
mkdir -p "$CL_STAGE/include/crosslink" "$CL_STAGE/lib/cmake/CrossLinkSDK" "$CL_STAGE/lib/pkgconfig" \
         "$CL_STAGE/apple" "$CL_STAGE/python" "$CL_STAGE/examples" "$ROOT_CL/usr/local/lib"

cp "$BUILD_DIR/install-arm64/include/crosslink/"* "$CL_STAGE/include/crosslink/"
cp "$UNIV_LIB_DIR/libcrosslink_core.dylib" "$CL_STAGE/lib/"
cp "$UNIV_LIB_DIR/libcrosslink_core.dylib" "$ROOT_CL/usr/local/lib/libcrosslink_core.dylib"
cp "$UNIV_LIB_DIR/libcrosslink_core.a"     "$CL_STAGE/lib/"
[ -f "$UNIV_LIB_DIR/libopus.a" ] && cp "$UNIV_LIB_DIR/libopus.a" "$CL_STAGE/lib/"
cp "$BUILD_DIR/install-arm64/lib/cmake/CrossLinkSDK/"* "$CL_STAGE/lib/cmake/CrossLinkSDK/"
sed "s|^prefix=.*|prefix=/usr/local/CrossLinkSDK|" \
  "$BUILD_DIR/install-arm64/lib/pkgconfig/crosslink-sdk.pc" > "$CL_STAGE/lib/pkgconfig/crosslink-sdk.pc"
[ -d "$DIST_APPLE/CrossLinkCore.xcframework" ] && cp -R "$DIST_APPLE/CrossLinkCore.xcframework" "$CL_STAGE/apple/"
cp -R "$APPLE_DIR/CrossLinkKit" "$CL_STAGE/apple/"
cp "$ROOT/sdk/python/crosslink_sdk.py" "$CL_STAGE/python/"
[ -d "$ROOT/sdk/examples/cpp_audio_client" ] && cp -R "$ROOT/sdk/examples/cpp_audio_client" "$CL_STAGE/examples/"
cp "$ROOT/sdk/README.md" "$CL_STAGE/README.md"

# Per-protocol Package.swift manifests so each installed SDK works standalone in Xcode
cat > "$LV_STAGE/apple/Package.swift" <<'EOF'
// swift-tools-version: 5.9
import PackageDescription
let package = Package(
    name: "LanVidKit",
    platforms: [.iOS(.v15), .macOS(.v12)],
    products: [.library(name: "LanVidKit", targets: ["LanVidKit"])],
    targets: [
        .binaryTarget(name: "LanVidCore", path: "LanVidCore.xcframework"),
        .target(name: "LanVidKit", dependencies: ["LanVidCore"], path: "LanVidKit/Sources/LanVidKit",
                linkerSettings: [
                    .linkedFramework("CoreVideo"), .linkedFramework("CoreMedia"),
                    .linkedFramework("VideoToolbox"), .linkedFramework("AVFoundation"),
                    .linkedFramework("Metal"), .linkedFramework("MetalKit"),
                    .linkedFramework("SystemConfiguration", .when(platforms: [.macOS])),
                    .linkedLibrary("c++")
                ])
    ]
)
EOF

cat > "$CL_STAGE/apple/Package.swift" <<'EOF'
// swift-tools-version: 5.9
import PackageDescription
let package = Package(
    name: "CrossLinkKit",
    platforms: [.iOS(.v15), .macOS(.v12)],
    products: [.library(name: "CrossLinkKit", targets: ["CrossLinkKit"])],
    targets: [
        .binaryTarget(name: "CrossLinkCore", path: "CrossLinkCore.xcframework"),
        .target(name: "CrossLinkKit", dependencies: ["CrossLinkCore"], path: "CrossLinkKit/Sources/CrossLinkKit",
                linkerSettings: [
                    .linkedFramework("CoreMIDI"),
                    .linkedFramework("CoreAudio", .when(platforms: [.macOS])),
                    .linkedFramework("SystemConfiguration", .when(platforms: [.macOS])),
                    .linkedLibrary("c++")
                ])
    ]
)
EOF

SCRIPTS_LV="$BUILD_DIR/scripts-lanvid"
SCRIPTS_CL="$BUILD_DIR/scripts-crosslink"
mkdir -p "$SCRIPTS_LV" "$SCRIPTS_CL"
cp "$INST_DIR/scripts/lanvid/preinstall" "$INST_DIR/scripts/lanvid/postinstall" "$SCRIPTS_LV/"
cp "$INST_DIR/scripts/crosslink/preinstall" "$INST_DIR/scripts/crosslink/postinstall" "$SCRIPTS_CL/"
chmod 0755 "$SCRIPTS_LV/"* "$SCRIPTS_CL/"*

echo "=== [5/6] Packaging selectable protocol .pkgs and unified macOS installer ==="
COMP_DIR="$BUILD_DIR/components"
mkdir -p "$COMP_DIR"

pkgbuild --root "$ROOT_LV" --identifier "com.lanvid.sdk.video" --version "$LANVID_VERSION" \
  --scripts "$SCRIPTS_LV" --install-location "/" "$COMP_DIR/lanvid-video.pkg"

pkgbuild --root "$ROOT_CL" --identifier "com.crosslink.sdk.audio" --version "$CROSSLINK_VERSION" \
  --scripts "$SCRIPTS_CL" --install-location "/" "$COMP_DIR/crosslink-audio.pkg"

SIGN_ID="${LANVID_INSTALLER_SIGN_ID:-}"
if [ -z "$SIGN_ID" ] && command -v security >/dev/null 2>&1; then
  SIGN_ID="$(security find-identity -v -p basic 2>/dev/null | grep 'Developer ID Installer:' | head -n 1 | sed -E 's/.*"([^"]+)".*/\1/' || true)"
fi

PROD_ARGS=(
  --distribution "$INST_DIR/distribution.xml"
  --resources "$INST_DIR/resources"
  --package-path "$COMP_DIR"
  --version "$BUNDLE_VERSION"
)
if [ -n "$SIGN_ID" ]; then
  echo "[INFO] Signing installer with: $SIGN_ID"
  PROD_ARGS+=(--sign "$SIGN_ID")
else
  echo "[INFO] No Developer ID Installer certificate found -> building unsigned local .pkg"
fi

productbuild "${PROD_ARGS[@]}" "$PKG_OUT"
echo "[OK] Created selectable macOS installer: $PKG_OUT ($(du -h "$PKG_OUT" | awk '{print $1}'))"

echo "=== [6/6] Creating Finder disk image: LanVidCrossLinkSDK-v${BUNDLE_VERSION}-macOS.dmg ==="
DMG_STAGE="$BUILD_DIR/dmg-stage"
mkdir -p "$DMG_STAGE"
cp "$PKG_OUT" "$DMG_STAGE/"
cp "$ROOT/sdk/README.md" "$DMG_STAGE/README.md"

cat > "$DMG_STAGE/Verify_Installed_SDKs.command" <<'VEOF'
#!/usr/bin/env bash
FOUND=0
if command -v lanvid-sdk-info >/dev/null 2>&1; then
  lanvid-sdk-info
  FOUND=1
fi
if command -v crosslink-sdk-info >/dev/null 2>&1; then
  crosslink-sdk-info
  FOUND=1
fi
if [ "$FOUND" -eq 0 ]; then
  echo "Neither LanVid Video SDK nor CrossLink Audio SDK is installed yet."
  echo "Double-click LanVidCrossLinkSDK-v3.1.0-macOS.pkg first."
fi
read -r -p "Press Enter to close..."
VEOF
chmod 0755 "$DMG_STAGE/Verify_Installed_SDKs.command"

DMG_OK=0
for attempt in 1 2 3 4 5; do
  if hdiutil create \
      -volname "LanVid & CrossLink SDK v${BUNDLE_VERSION}" \
      -srcfolder "$DMG_STAGE" \
      -ov -format UDZO \
      "$DMG_OUT" >/dev/null; then
    DMG_OK=1
    break
  fi
  echo "[WARN] hdiutil create busy (attempt $attempt/5), retrying in 3s..."
  sleep 3
done
if [ "$DMG_OK" -ne 1 ]; then
  echo "[ERROR] hdiutil create failed after 5 attempts" >&2
  exit 1
fi

echo "========================================================"
echo "  SUCCESS! macOS Selectable SDK Installers Ready:"
echo "    PKG: $PKG_OUT"
echo "    DMG: $DMG_OUT"
echo "========================================================"
