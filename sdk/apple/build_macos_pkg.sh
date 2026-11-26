#!/usr/bin/env bash
# LanVid Core SDK macOS Single-File Installer Builder (Universal arm64 NEON + x86_64 AVX2)
# Produces:
#   - dist/apple/LanVidSDK-v2.7.0-macOS.pkg
#   - dist/apple/LanVidSDK-v2.7.0-macOS.dmg
#
# Packages:
#   - com.lanvid.sdk.core   (mandatory): liblanvid_core.{dylib,a}, headers, CMake/pkg-config, LanVidCore.xcframework
#   - com.lanvid.sdk.swift  (optional):  Swift Package LanVidKit
#   - com.lanvid.sdk.extras (optional):  Python ctypes bindings + C++/Swift examples

set -euo pipefail

SDK_VERSION="2.7.0"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPLE_DIR="$ROOT/sdk/apple"
INST_DIR="$APPLE_DIR/installer"
BUILD_DIR="$ROOT/build-macos-pkg"
DIST_APPLE="$ROOT/dist/apple"
PKG_OUT="$DIST_APPLE/LanVidSDK-v${SDK_VERSION}-macOS.pkg"
DMG_OUT="$DIST_APPLE/LanVidSDK-v${SDK_VERSION}-macOS.dmg"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "[ERROR] build_macos_pkg.sh must be executed on macOS (uses lipo, pkgbuild, productbuild, hdiutil)." >&2
  exit 1
fi

for req in cmake lipo install_name_tool pkgbuild productbuild hdiutil; do
  if ! command -v "$req" >/dev/null 2>&1; then
    echo "[ERROR] Required macOS tool '$req' not found. Install Xcode Command Line Tools & CMake." >&2
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

  echo "=== Building macOS $arch (NEON=$neon, AVX2=$avx2) ==="
  cmake -S "$ROOT" -B "$bdir" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DLANVID_SDK_CORE_ONLY=ON \
    -DLANVID_BUILD_SHARED=ON \
    -DLANVID_BUILD_TESTS=OFF \
    -DLANVID_ENABLE_NEON="$neon" \
    -DLANVID_ENABLE_AVX2="$avx2"
  cmake --build "$bdir" --target lanvid_core lanvid_core_shared --parallel "$NCPU"
  cmake --install "$bdir" --prefix "$idir"
}

echo "=== [2/6] Compiling Universal macOS SDK libraries (arm64 NEON + x86_64 AVX2) ==="
build_macos_arch "arm64"  "ON"  "OFF"
build_macos_arch "x86_64" "OFF" "ON"

UNIV_LIB_DIR="$BUILD_DIR/universal-lib"
mkdir -p "$UNIV_LIB_DIR"

lipo -create \
  "$BUILD_DIR/install-arm64/lib/liblanvid_core.dylib" \
  "$BUILD_DIR/install-x86_64/lib/liblanvid_core.dylib" \
  -output "$UNIV_LIB_DIR/liblanvid_core.dylib"

lipo -create \
  "$BUILD_DIR/install-arm64/lib/liblanvid_core.a" \
  "$BUILD_DIR/install-x86_64/lib/liblanvid_core.a" \
  -output "$UNIV_LIB_DIR/liblanvid_core.a"

install_name_tool -id "/usr/local/lib/liblanvid_core.dylib" "$UNIV_LIB_DIR/liblanvid_core.dylib"
if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$UNIV_LIB_DIR/liblanvid_core.dylib"
else
  codesign --force --sign - "$UNIV_LIB_DIR/liblanvid_core.dylib"
fi
echo "[OK] Universal dylib slices: $(lipo -archs "$UNIV_LIB_DIR/liblanvid_core.dylib")"

echo "=== [3/6] Building LanVidCore.xcframework (macOS + iOS + Simulator) ==="
if [ -z "${LANVID_SKIP_XCFRAMEWORK:-}" ]; then
  bash "$APPLE_DIR/build_xcframework.sh"
fi

echo "=== [4/6] Staging SDK installer component roots ==="
ROOT_CORE="$BUILD_DIR/pkgroot-core"
SDK_STAGE="$ROOT_CORE/usr/local/LanVidSDK"
mkdir -p "$SDK_STAGE/include/lanvid" "$SDK_STAGE/lib/cmake/LanVidSDK" "$SDK_STAGE/lib/pkgconfig" \
         "$SDK_STAGE/apple" "$ROOT_CORE/usr/local/lib"

cp "$BUILD_DIR/install-arm64/include/lanvid/"* "$SDK_STAGE/include/lanvid/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$SDK_STAGE/lib/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$ROOT_CORE/usr/local/lib/liblanvid_core.dylib"
cp "$UNIV_LIB_DIR/liblanvid_core.a"     "$SDK_STAGE/lib/"
cp "$BUILD_DIR/install-arm64/lib/cmake/LanVidSDK/"* "$SDK_STAGE/lib/cmake/LanVidSDK/"
sed "s|^prefix=.*|prefix=/usr/local/LanVidSDK|" \
  "$BUILD_DIR/install-arm64/lib/pkgconfig/lanvid-sdk.pc" > "$SDK_STAGE/lib/pkgconfig/lanvid-sdk.pc"
if [ -d "$DIST_APPLE/LanVidCore.xcframework" ]; then
  cp -R "$DIST_APPLE/LanVidCore.xcframework" "$SDK_STAGE/apple/"
fi
cp "$ROOT/sdk/README.md" "$SDK_STAGE/README.md"

ROOT_SWIFT="$BUILD_DIR/pkgroot-swift/usr/local/LanVidSDK"
ROOT_EXTRAS="$BUILD_DIR/pkgroot-extras/usr/local/LanVidSDK"
mkdir -p "$ROOT_SWIFT/apple" "$ROOT_EXTRAS/python" "$ROOT_EXTRAS/examples"
cp -R "$APPLE_DIR/LanVidKit" "$ROOT_SWIFT/apple/"
sed 's|../../dist/apple/LanVidCore.xcframework|LanVidCore.xcframework|g' \
  "$APPLE_DIR/Package.swift" > "$ROOT_SWIFT/apple/Package.swift"
cp "$ROOT/sdk/python/lanvid_sdk.py" "$ROOT_EXTRAS/python/"
cp -R "$APPLE_DIR/Examples" "$ROOT_EXTRAS/apple-examples"
cp -R "$ROOT/sdk/examples/cpp_client" "$ROOT_EXTRAS/examples/"

SCRIPTS_STAGE="$BUILD_DIR/scripts"
mkdir -p "$SCRIPTS_STAGE"
cp "$INST_DIR/scripts/preinstall" "$INST_DIR/scripts/postinstall" "$SCRIPTS_STAGE/"
chmod 0755 "$SCRIPTS_STAGE/preinstall" "$SCRIPTS_STAGE/postinstall"

echo "=== [5/6] Packaging component .pkgs and building LanVidSDK-v${SDK_VERSION}-macOS.pkg ==="
COMP_DIR="$BUILD_DIR/components"
mkdir -p "$COMP_DIR"

pkgbuild --root "$ROOT_CORE" --identifier "com.lanvid.sdk.core" --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_STAGE" --install-location "/" "$COMP_DIR/lanvid-core.pkg"

pkgbuild --root "$BUILD_DIR/pkgroot-swift" --identifier "com.lanvid.sdk.swift" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-swift.pkg"

pkgbuild --root "$BUILD_DIR/pkgroot-extras" --identifier "com.lanvid.sdk.extras" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-extras.pkg"

SIGN_ID="${LANVID_INSTALLER_SIGN_ID:-}"
if [ -z "$SIGN_ID" ] && command -v security >/dev/null 2>&1; then
  SIGN_ID="$(security find-identity -v -p basic 2>/dev/null | grep 'Developer ID Installer:' | head -n 1 | sed -E 's/.*"([^"]+)".*/\1/' || true)"
fi

PROD_ARGS=(
  --distribution "$INST_DIR/distribution.xml"
  --resources "$INST_DIR/resources"
  --package-path "$COMP_DIR"
  --version "$SDK_VERSION"
)
if [ -n "$SIGN_ID" ]; then
  echo "[INFO] Signing installer with: $SIGN_ID"
  PROD_ARGS+=(--sign "$SIGN_ID")
else
  echo "[INFO] No Developer ID Installer certificate found -> building unsigned local .pkg"
fi

productbuild "${PROD_ARGS[@]}" "$PKG_OUT"
echo "[OK] Created installer package: $PKG_OUT ($(du -h "$PKG_OUT" | awk '{print $1}'))"

echo "=== [6/6] Creating Finder disk image: LanVidSDK-v${SDK_VERSION}-macOS.dmg ==="
DMG_STAGE="$BUILD_DIR/dmg-stage"
mkdir -p "$DMG_STAGE"
cp "$PKG_OUT" "$DMG_STAGE/"
cp "$ROOT/sdk/README.md" "$DMG_STAGE/README.md"

cat > "$DMG_STAGE/Verify_LanVidSDK.command" <<'VEOF'
#!/usr/bin/env bash
if command -v lanvid-sdk-info >/dev/null 2>&1; then
  lanvid-sdk-info
else
  echo "LanVidSDK is not installed yet. Double-click LanVidSDK-v2.7.0-macOS.pkg first."
fi
read -r -p "Press Enter to close..."
VEOF
chmod 0755 "$DMG_STAGE/Verify_LanVidSDK.command"

DMG_OK=0
for attempt in 1 2 3 4 5; do
  if hdiutil create \
      -volname "LanVidSDK v${SDK_VERSION}" \
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
echo "  SUCCESS! macOS SDK Installers Ready:"
echo "    PKG: $PKG_OUT"
echo "    DMG: $DMG_OUT"
echo "========================================================"
