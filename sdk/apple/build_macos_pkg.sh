#!/usr/bin/env bash
# LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server + SDK macOS Single-File Installer Builder
# Produces:
#   - dist/apple/LanVidSDK-v2.6.0-macOS.pkg  (Universal arm64 NEON + x86_64 AVX2 Installer.app package)
#   - dist/apple/LanVidSDK-v2.6.0-macOS.dmg  (Drag/double-click Finder disk image with .pkg + Verify script)
#
# Installs out of the box:
#   1. LanVid Studio.app -> /Applications/ (when built), lanvid_dsp_server -> /usr/local/bin/lanvid_dsp_server,
#      liblanvid_core.dylib -> /usr/local/lib/liblanvid_core.dylib
#   2. LanVidAudio.driver (64ch CoreAudio HAL) -> /Library/Audio/Plug-Ins/HAL/
#   3. LanVid Grid DSP Server LaunchDaemon -> /Library/LaunchDaemons/com.lanvid.dspserver.plist (KeepAlive + RunAtLoad)
#   4. LanVid Grid L2 ChmodBPF LaunchDaemon -> /Library/LaunchDaemons/com.lanvid.chmod-bpf.plist
#      (grants access_bpf group rw on /dev/bpf* and adds console user to access_bpf for rootless L2 Ethernet)
#   5. Automatic OBS Studio plugin installation (obs-lanvid.plugin -> ~/Library/Application Support/obs-studio/plugins/)
#   6. macOS Application Firewall exemptions via socketfilterfw (--add & --unblockapp)
#
# Run on macOS (Apple Silicon M1-M4 or Intel Mac) with Xcode Command Line Tools + CMake:
#   ./sdk/apple/build_macos_pkg.sh
# Optional signing:
#   LANVID_INSTALLER_SIGN_ID="Developer ID Installer: Your Name (TEAMID)" ./sdk/apple/build_macos_pkg.sh

set -euo pipefail

SDK_VERSION="2.6.0"
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

echo "=== [1/7] Cleaning staging directories ==="
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
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON \
    -DLANVID_SDK_CORE_ONLY=OFF \
    -DLANVID_BUILD_SHARED=ON \
    -DLANVID_BUILD_GUI=OFF \
    -DLANVID_BUILD_CLI=ON \
    -DLANVID_BUILD_DRIVERS=OFF \
    -DLANVID_BUILD_TESTS=OFF \
    -DBUILD_OBS_PLUGIN=OFF \
    -DLANVID_ENABLE_NEON="$neon" \
    -DLANVID_ENABLE_AVX2="$avx2"
  cmake --build "$bdir" --target lanvid_core lanvid_core_shared lanvid_dsp_server --parallel "$NCPU"
  cmake --install "$bdir" --prefix "$idir"
}

echo "=== [2/7] Compiling Universal macOS libraries & lanvid_dsp_server (arm64 NEON + x86_64 AVX2) ==="
build_macos_arch "arm64"  "ON"  "OFF"
build_macos_arch "x86_64" "OFF" "ON"

UNIV_LIB_DIR="$BUILD_DIR/universal-lib"
UNIV_BIN_DIR="$BUILD_DIR/universal-bin"
mkdir -p "$UNIV_LIB_DIR" "$UNIV_BIN_DIR"

lipo -create \
  "$BUILD_DIR/install-arm64/lib/liblanvid_core.dylib" \
  "$BUILD_DIR/install-x86_64/lib/liblanvid_core.dylib" \
  -output "$UNIV_LIB_DIR/liblanvid_core.dylib"

lipo -create \
  "$BUILD_DIR/install-arm64/lib/liblanvid_core.a" \
  "$BUILD_DIR/install-x86_64/lib/liblanvid_core.a" \
  -output "$UNIV_LIB_DIR/liblanvid_core.a"

lipo -create \
  "$BUILD_DIR/macos-arm64/lanvid_dsp_server" \
  "$BUILD_DIR/macos-x86_64/lanvid_dsp_server" \
  -output "$UNIV_BIN_DIR/lanvid_dsp_server"

# Set canonical install_name and sign (mandatory on Apple Silicon arm64!)
install_name_tool -id "/usr/local/lib/liblanvid_core.dylib" "$UNIV_LIB_DIR/liblanvid_core.dylib"
if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$UNIV_LIB_DIR/liblanvid_core.dylib"
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$UNIV_BIN_DIR/lanvid_dsp_server"
else
  codesign --force --sign - "$UNIV_LIB_DIR/liblanvid_core.dylib"
  codesign --force --sign - "$UNIV_BIN_DIR/lanvid_dsp_server"
fi
echo "[OK] Universal dylib slices      : $(lipo -archs "$UNIV_LIB_DIR/liblanvid_core.dylib")"
echo "[OK] Universal dsp_server slices : $(lipo -archs "$UNIV_BIN_DIR/lanvid_dsp_server")"

echo "=== [3/7] Building LanVidCore.xcframework (macOS + iOS + Simulator) ==="
if [ -z "${LANVID_SKIP_XCFRAMEWORK:-}" ]; then
  bash "$APPLE_DIR/build_xcframework.sh"
fi

echo "=== [4/7] Staging installer component roots ==="
ROOT_CORE="$BUILD_DIR/pkgroot-core/usr/local/LanVidSDK"
ROOT_SWIFT="$BUILD_DIR/pkgroot-swift/usr/local/LanVidSDK"
ROOT_EXTRAS="$BUILD_DIR/pkgroot-extras/usr/local/LanVidSDK"
mkdir -p "$ROOT_CORE/include/lanvid" "$ROOT_CORE/lib/cmake/LanVidSDK" "$ROOT_CORE/lib/pkgconfig" \
         "$ROOT_CORE/apple" "$BUILD_DIR/pkgroot-core/usr/local/bin" "$BUILD_DIR/pkgroot-core/usr/local/lib" \
         "$ROOT_SWIFT/apple" "$ROOT_EXTRAS/python" "$ROOT_EXTRAS/examples"

# Component 1: Core C/C++17, Universal dylib & static lib, lanvid_dsp_server, optional LanVid Studio.app, XCFramework
cp "$BUILD_DIR/install-arm64/include/lanvid/"* "$ROOT_CORE/include/lanvid/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$ROOT_CORE/lib/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$BUILD_DIR/pkgroot-core/usr/local/lib/liblanvid_core.dylib"
cp "$UNIV_LIB_DIR/liblanvid_core.a"     "$ROOT_CORE/lib/"
cp "$UNIV_BIN_DIR/lanvid_dsp_server"    "$BUILD_DIR/pkgroot-core/usr/local/bin/lanvid_dsp_server"
chmod 0755 "$BUILD_DIR/pkgroot-core/usr/local/bin/lanvid_dsp_server"
cp "$BUILD_DIR/install-arm64/lib/cmake/LanVidSDK/"* "$ROOT_CORE/lib/cmake/LanVidSDK/"

# Bundle LanVid Studio.app -> /Applications/ when prebuilt (e.g. build-macos-app or build-studio-pkg)
STUDIO_APP_SRC=""
for cand in "$ROOT/build-macos-app/LanVid Studio.app" "$ROOT/build-studio-pkg/cmake/LanVid Studio.app" "$ROOT/build/LanVid Studio.app"; do
  if [ -d "$cand" ]; then
    STUDIO_APP_SRC="$cand"
    break
  fi
done
if [ -n "$STUDIO_APP_SRC" ]; then
  mkdir -p "$BUILD_DIR/pkgroot-core/Applications"
  cp -R "$STUDIO_APP_SRC" "$BUILD_DIR/pkgroot-core/Applications/"
  echo "[OK] Staged LanVid Studio.app -> /Applications/ from $STUDIO_APP_SRC"
fi

# Rewrite installed prefix inside lanvid-sdk.pc to canonical /usr/local/LanVidSDK
sed "s|^prefix=.*|prefix=/usr/local/LanVidSDK|" \
  "$BUILD_DIR/install-arm64/lib/pkgconfig/lanvid-sdk.pc" > "$ROOT_CORE/lib/pkgconfig/lanvid-sdk.pc"

if [ -d "$DIST_APPLE/LanVidCore.xcframework" ]; then
  cp -R "$DIST_APPLE/LanVidCore.xcframework" "$ROOT_CORE/apple/"
fi
cp "$ROOT/sdk/README.md" "$ROOT_CORE/README.md"

# Component 2: Swift Package LanVidKit (adjust binaryTarget path for installed /usr/local/LanVidSDK/apple layout)
cp -R "$APPLE_DIR/LanVidKit" "$ROOT_SWIFT/apple/"
sed 's|../../dist/apple/LanVidCore.xcframework|LanVidCore.xcframework|g' \
  "$APPLE_DIR/Package.swift" > "$ROOT_SWIFT/apple/Package.swift"

# Component 3: Python Bindings, Examples & obs-lanvid plugin + locales
cp "$ROOT/sdk/python/lanvid_sdk.py" "$ROOT_EXTRAS/python/"
cp -R "$APPLE_DIR/Examples" "$ROOT_EXTRAS/apple-examples"
cp -R "$ROOT/sdk/examples/cpp_client" "$ROOT_EXTRAS/examples/"

if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  mkdir -p "$ROOT_EXTRAS/obs-lanvid/locale"
  cp "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$ROOT_EXTRAS/obs-lanvid/locale/"
fi
for obs_cand in "$ROOT/build/plugins/obs-lanvid/obs-lanvid.plugin" "$ROOT/build-macos-app/plugins/obs-lanvid/obs-lanvid.plugin"; do
  if [ -d "$obs_cand" ]; then
    mkdir -p "$ROOT_EXTRAS/obs-lanvid"
    cp -R "$obs_cand" "$ROOT_EXTRAS/obs-lanvid/"
    break
  fi
done

echo "=== [4b/7] Building LanVidAudio.driver (CoreAudio HAL plug-in, Universal arm64 + x86_64) ==="
HAL_BUILD="$BUILD_DIR/hal"
cmake -S "$ROOT" -B "$HAL_BUILD" -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DLANVID_BUILD_GUI=OFF -DLANVID_BUILD_CLI=OFF -DLANVID_BUILD_TESTS=OFF -DLANVID_BUILD_DRIVERS=ON
cmake --build "$HAL_BUILD" --target LanVidAudio --parallel "$NCPU"
HAL_BUNDLE="$HAL_BUILD/LanVidAudio.driver"
[ -f "$HAL_BUNDLE/Contents/MacOS/LanVidAudio" ] || { echo "[ERROR] LanVidAudio.driver missing" >&2; exit 1; }
if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$HAL_BUNDLE"
else
  codesign --force --sign - "$HAL_BUNDLE"
fi
echo "[OK] LanVidAudio.driver slices: $(lipo -archs "$HAL_BUNDLE/Contents/MacOS/LanVidAudio")"
ROOT_HAL="$BUILD_DIR/pkgroot-hal/Library/Audio/Plug-Ins/HAL"
mkdir -p "$ROOT_HAL"
cp -R "$HAL_BUNDLE" "$ROOT_HAL/"
HAL_SCRIPTS="$BUILD_DIR/scripts-hal"
mkdir -p "$HAL_SCRIPTS"
cat > "$HAL_SCRIPTS/postinstall" <<'HALEOF'
#!/bin/bash
# Reload the HAL so "LanVid Virtual 64ch" appears in Audio MIDI Setup / DAWs (launchd restarts coreaudiod).
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/LanVidAudio.driver" 2>/dev/null || true
killall -9 coreaudiod 2>/dev/null || true
exit 0
HALEOF
chmod 0755 "$HAL_SCRIPTS/postinstall"

echo "=== [4c/7] Staging LanVid Grid L2 (ChmodBPF) & DSP Server LaunchDaemons ==="
ROOT_GRID="$BUILD_DIR/pkgroot-grid"
mkdir -p "$ROOT_GRID/Library/LaunchDaemons" \
         "$ROOT_GRID/Library/Application Support/LanVid/ChmodBPF"

# Helper script run by com.lanvid.chmod-bpf.plist at every boot (grants access_bpf rw on /dev/bpf*)
cat > "$ROOT_GRID/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" <<'BPFEOF'
#!/bin/bash
# LanVid Grid Layer 2 BPF permission helper (analogous to Wireshark ChmodBPF).
# Allows non-root users in group `access_bpf` to open /dev/bpf* for sub-ms Layer 2 Ethernet audio.
set -e
GROUP="access_bpf"
if ! dscl . -read "/Groups/$GROUP" >/dev/null 2>&1; then
  dseditgroup -q -o create "$GROUP" || true
fi
chgrp "$GROUP" /dev/bpf* 2>/dev/null || true
chmod g+rw /dev/bpf* 2>/dev/null || true
exit 0
BPFEOF
chmod 0755 "$ROOT_GRID/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"

# LaunchDaemon 1: /Library/LaunchDaemons/com.lanvid.chmod-bpf.plist
cat > "$ROOT_GRID/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" <<'PLISTBPF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.lanvid.chmod-bpf</string>
  <key>ProgramArguments</key>
  <array>
    <string>/Library/Application Support/LanVid/ChmodBPF/ChmodBPF</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
</dict>
</plist>
PLISTBPF

# LaunchDaemon 2: /Library/LaunchDaemons/com.lanvid.dspserver.plist (KeepAlive + RunAtLoad)
cat > "$ROOT_GRID/Library/LaunchDaemons/com.lanvid.dspserver.plist" <<'PLISTDSP'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.lanvid.dspserver</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/lanvid_dsp_server</string>
    <string>--name</string>
    <string>LanVid Grid DSP</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>
  <key>StandardOutPath</key>
  <string>/var/log/lanvid-dsp.log</string>
  <key>StandardErrorPath</key>
  <string>/var/log/lanvid-dsp.log</string>
</dict>
</plist>
PLISTDSP
chmod 0644 "$ROOT_GRID/Library/LaunchDaemons/"*.plist

GRID_SCRIPTS="$BUILD_DIR/scripts-grid"
mkdir -p "$GRID_SCRIPTS"
cat > "$GRID_SCRIPTS/postinstall" <<'GRIDPOST'
#!/usr/bin/env bash
set -euo pipefail

CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then
  CONSOLE_USER="root"
fi

# 1. Configure access_bpf group, add current user, and apply /dev/bpf* rw permissions
if ! dscl . -read /Groups/access_bpf >/dev/null 2>&1; then
  dseditgroup -q -o create access_bpf || true
fi
if [ "$CONSOLE_USER" != "root" ]; then
  dseditgroup -q -o edit -a "$CONSOLE_USER" -t user access_bpf || true
fi
chown -R root:wheel "/Library/Application Support/LanVid/ChmodBPF" 2>/dev/null || true
chmod 0755 "/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" 2>/dev/null || true
"/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" || true

# 2. Load LaunchDaemons (com.lanvid.chmod-bpf.plist & com.lanvid.dspserver.plist)
for plist in "/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" "/Library/LaunchDaemons/com.lanvid.dspserver.plist"; do
  if [ -f "$plist" ]; then
    chown root:wheel "$plist" 2>/dev/null || true
    chmod 0644 "$plist" 2>/dev/null || true
    launchctl bootout system "$plist" 2>/dev/null || launchctl unload -w "$plist" 2>/dev/null || true
    launchctl bootstrap system "$plist" 2>/dev/null || launchctl load -w "$plist" 2>/dev/null || true
  fi
done

# 3. Auto-install OBS Studio plugin (obs-lanvid.plugin) if OBS Studio is detected
if [ "$CONSOLE_USER" != "root" ]; then
  USER_HOME="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}' || echo "/Users/$CONSOLE_USER")"
  OBS_USER_DIR="$USER_HOME/Library/Application Support/obs-studio"
  if [ -d "/Applications/OBS.app" ] || [ -d "$OBS_USER_DIR" ]; then
    OBS_PLUGIN_DST="$OBS_USER_DIR/plugins"
    mkdir -p "$OBS_PLUGIN_DST"
    if [ -d "/usr/local/LanVidSDK/obs-lanvid/obs-lanvid.plugin" ]; then
      rm -rf "$OBS_PLUGIN_DST/obs-lanvid.plugin"
      cp -R "/usr/local/LanVidSDK/obs-lanvid/obs-lanvid.plugin" "$OBS_PLUGIN_DST/"
    fi
    if [ -d "/usr/local/LanVidSDK/obs-lanvid/locale" ]; then
      mkdir -p "$OBS_PLUGIN_DST/obs-lanvid.plugin/Contents/Resources/locale"
      cp "/usr/local/LanVidSDK/obs-lanvid/locale/"*.ini "$OBS_PLUGIN_DST/obs-lanvid.plugin/Contents/Resources/locale/" 2>/dev/null || true
    fi
    chown -R "$CONSOLE_USER":staff "$OBS_USER_DIR" 2>/dev/null || true
  fi
fi

# 4. macOS Application Firewall exemptions (socketfilterfw)
FW="/usr/libexec/ApplicationFirewall/socketfilterfw"
if [ -x "$FW" ]; then
  for target in \
      "/Applications/LanVid Studio.app" \
      "/Applications/LanVid Studio.app/Contents/MacOS/LanVid Studio" \
      "/usr/local/bin/lanvid_dsp_server" \
      "/usr/local/lib/liblanvid_core.dylib"; do
    if [ -e "$target" ]; then
      "$FW" --add "$target" >/dev/null 2>&1 || true
      "$FW" --unblockapp "$target" >/dev/null 2>&1 || true
    fi
  done
fi
exit 0
GRIDPOST
chmod 0755 "$GRID_SCRIPTS/postinstall"

# Ensure installer scripts have +x and Unix LF endings
SCRIPTS_STAGE="$BUILD_DIR/scripts"
mkdir -p "$SCRIPTS_STAGE"
cp "$INST_DIR/scripts/preinstall" "$INST_DIR/scripts/postinstall" "$SCRIPTS_STAGE/"
chmod 0755 "$SCRIPTS_STAGE/preinstall" "$SCRIPTS_STAGE/postinstall"

echo "=== [5/7] Packaging component .pkgs and building final LanVidSDK-v${SDK_VERSION}-macOS.pkg ==="
COMP_DIR="$BUILD_DIR/components"
mkdir -p "$COMP_DIR"

pkgbuild \
  --root "$BUILD_DIR/pkgroot-core" \
  --identifier "com.lanvid.sdk.core" \
  --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_STAGE" \
  --install-location "/" \
  "$COMP_DIR/lanvid-core.pkg"

pkgbuild \
  --root "$BUILD_DIR/pkgroot-swift" \
  --identifier "com.lanvid.sdk.swift" \
  --version "$SDK_VERSION" \
  --install-location "/" \
  "$COMP_DIR/lanvid-swift.pkg"

pkgbuild \
  --root "$BUILD_DIR/pkgroot-extras" \
  --identifier "com.lanvid.sdk.extras" \
  --version "$SDK_VERSION" \
  --install-location "/" \
  "$COMP_DIR/lanvid-extras.pkg"

pkgbuild \
  --root "$BUILD_DIR/pkgroot-hal" \
  --identifier "com.lanvid.audio.hal" \
  --version "$SDK_VERSION" \
  --scripts "$HAL_SCRIPTS" \
  --install-location "/" \
  "$COMP_DIR/lanvid-hal.pkg"

pkgbuild \
  --root "$ROOT_GRID" \
  --identifier "com.lanvid.grid.system" \
  --version "$SDK_VERSION" \
  --scripts "$GRID_SCRIPTS" \
  --install-location "/" \
  "$COMP_DIR/lanvid-grid.pkg"

# Detect optional Developer ID Installer certificate in Keychain (falls back to unsigned local pkg)
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

echo "=== [6/7] Creating Finder disk image: LanVidSDK-v${SDK_VERSION}-macOS.dmg ==="
DMG_STAGE="$BUILD_DIR/dmg-stage"
mkdir -p "$DMG_STAGE"
cp "$PKG_OUT" "$DMG_STAGE/"
cp "$ROOT/sdk/README.md" "$DMG_STAGE/README.md"
if [ -n "$STUDIO_APP_SRC" ]; then
  cp -R "$STUDIO_APP_SRC" "$DMG_STAGE/"
fi

cat > "$DMG_STAGE/Verify_LanVidSDK.command" <<'VEOF'
#!/usr/bin/env bash
if command -v lanvid-sdk-info >/dev/null 2>&1; then
  lanvid-sdk-info
else
  echo "LanVidSDK is not installed yet. Double-click LanVidSDK-v2.6.0-macOS.pkg first."
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

echo "=== [7/7] Done ==="
echo "========================================================"
echo "  SUCCESS! macOS Single-File Installers Ready:"
echo "    PKG: $PKG_OUT"
echo "    DMG: $DMG_OUT"
echo "========================================================"
