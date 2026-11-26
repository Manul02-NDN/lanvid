#!/usr/bin/env bash
# LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server + SDK macOS Single-File Installer Builder
# Produces:
#   - dist/apple/LanVidSDK-v2.6.0-macOS.pkg  (Universal arm64 NEON + x86_64 AVX2 Installer.app package)
#   - dist/apple/LanVidSDK-v2.6.0-macOS.dmg  (Drag/double-click Finder disk image with .pkg + Verify script)
#
# Supports BOTH Installer.app modes via distribution.xml (customize="allow"):
#   - Standard Install ("Інсталювати"): automatically installs all packages (Studio + DSP Server + ChmodBPF + OBS + HAL)
#   - Customize ("Налаштувати"): lets users toggle individual optional components:
#       * com.lanvid.pkg.studio     (mandatory, enabled="false"): LanVid Studio.app + liblanvid_core.dylib + SDK + Firewall
#       * com.lanvid.pkg.dspserver  (optional): /usr/local/bin/lanvid_dsp_server + com.lanvid.dspserver.plist (KeepAlive)
#       * com.lanvid.pkg.chmodbpf   (optional): ChmodBPF helper + com.lanvid.chmod-bpf.plist (access_bpf -> /dev/bpf*)
#       * com.lanvid.pkg.obsplugin  (optional): obs-lanvid.plugin -> ~/Library/Application Support/obs-studio/plugins/
#       * com.lanvid.audio.hal      (optional): LanVidAudio.driver (64ch CoreAudio HAL)
#       * com.lanvid.pkg.vcam       (optional, when signed): LanVid Virtual Camera.app -> /Applications
#         (CoreMediaIO Camera Extension "LanVid Cam 1..8" + "LanVid Program Out"; built by
#         drivers/macos_vcam/build_vcam.sh, needs an Apple Development / Developer ID team in Xcode)
#       * com.lanvid.sdk.swift / com.lanvid.sdk.extras (optional)

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

echo "=== [4/7] Staging modular installer component roots (Standard & Customize) ==="
# Component 1 (mandatory): com.lanvid.pkg.studio -> LanVid Studio.app + liblanvid_core.dylib + Core SDK
ROOT_STUDIO="$BUILD_DIR/pkgroot-studio"
SDK_STAGE="$ROOT_STUDIO/usr/local/LanVidSDK"
mkdir -p "$SDK_STAGE/include/lanvid" "$SDK_STAGE/lib/cmake/LanVidSDK" "$SDK_STAGE/lib/pkgconfig" \
         "$SDK_STAGE/apple" "$ROOT_STUDIO/usr/local/lib"

cp "$BUILD_DIR/install-arm64/include/lanvid/"* "$SDK_STAGE/include/lanvid/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$SDK_STAGE/lib/"
cp "$UNIV_LIB_DIR/liblanvid_core.dylib" "$ROOT_STUDIO/usr/local/lib/liblanvid_core.dylib"
cp "$UNIV_LIB_DIR/liblanvid_core.a"     "$SDK_STAGE/lib/"
cp "$BUILD_DIR/install-arm64/lib/cmake/LanVidSDK/"* "$SDK_STAGE/lib/cmake/LanVidSDK/"
sed "s|^prefix=.*|prefix=/usr/local/LanVidSDK|" \
  "$BUILD_DIR/install-arm64/lib/pkgconfig/lanvid-sdk.pc" > "$SDK_STAGE/lib/pkgconfig/lanvid-sdk.pc"
if [ -d "$DIST_APPLE/LanVidCore.xcframework" ]; then
  cp -R "$DIST_APPLE/LanVidCore.xcframework" "$SDK_STAGE/apple/"
fi
cp "$ROOT/sdk/README.md" "$SDK_STAGE/README.md"

STUDIO_APP_SRC=""
for cand in "$ROOT/build-macos-app/LanVid Studio.app" "$ROOT/build-studio-pkg/cmake/LanVid Studio.app" "$ROOT/build/LanVid Studio.app"; do
  if [ -d "$cand" ]; then
    STUDIO_APP_SRC="$cand"
    break
  fi
done
if [ -n "$STUDIO_APP_SRC" ]; then
  mkdir -p "$ROOT_STUDIO/Applications"
  cp -R "$STUDIO_APP_SRC" "$ROOT_STUDIO/Applications/"
  echo "[OK] Staged LanVid Studio.app -> /Applications/ from $STUDIO_APP_SRC"
fi

# Component 1b (optional): com.lanvid.pkg.audiostudio -> /Applications/LanVid Audio.app
ROOT_AUDIO_APP="$BUILD_DIR/pkgroot-audiostudio"
mkdir -p "$ROOT_AUDIO_APP/Applications"
for cand in "$ROOT/build-macos-app/LanVid Audio.app" "$ROOT/build-studio-pkg/cmake/LanVid Audio.app" "$ROOT/build/LanVid Audio.app"; do
  if [ -d "$cand" ]; then
    cp -R "$cand" "$ROOT_AUDIO_APP/Applications/"
    echo "[OK] Staged LanVid Audio.app -> /Applications/ from $cand"
    break
  fi
done

# Component 1c (optional): com.lanvid.pkg.videostudio -> /Applications/LanVid Video.app
ROOT_VIDEO_APP="$BUILD_DIR/pkgroot-videostudio"
mkdir -p "$ROOT_VIDEO_APP/Applications"
for cand in "$ROOT/build-macos-app/LanVid Video.app" "$ROOT/build-studio-pkg/cmake/LanVid Video.app" "$ROOT/build/LanVid Video.app"; do
  if [ -d "$cand" ]; then
    cp -R "$cand" "$ROOT_VIDEO_APP/Applications/"
    echo "[OK] Staged LanVid Video.app -> /Applications/ from $cand"
    break
  fi
done

# Component 2 (optional): com.lanvid.pkg.dspserver -> /usr/local/bin/lanvid_dsp_server + LaunchDaemon
ROOT_DSP="$BUILD_DIR/pkgroot-dspserver"
mkdir -p "$ROOT_DSP/usr/local/bin" "$ROOT_DSP/Library/LaunchDaemons"
cp "$UNIV_BIN_DIR/lanvid_dsp_server" "$ROOT_DSP/usr/local/bin/lanvid_dsp_server"
chmod 0755 "$ROOT_DSP/usr/local/bin/lanvid_dsp_server"
cat > "$ROOT_DSP/Library/LaunchDaemons/com.lanvid.dspserver.plist" <<'PLISTDSP'
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
chmod 0644 "$ROOT_DSP/Library/LaunchDaemons/com.lanvid.dspserver.plist"

SCRIPTS_DSP="$BUILD_DIR/scripts-dspserver"
mkdir -p "$SCRIPTS_DSP"
cat > "$SCRIPTS_DSP/postinstall" <<'DSPPOST'
#!/usr/bin/env bash
set -euo pipefail
PLIST="/Library/LaunchDaemons/com.lanvid.dspserver.plist"
if [ -x "/usr/local/bin/lanvid_dsp_server" ] && [ -f "$PLIST" ]; then
  chown root:wheel "$PLIST" 2>/dev/null || true
  chmod 0644 "$PLIST" 2>/dev/null || true
  launchctl bootout system "$PLIST" 2>/dev/null || launchctl unload -w "$PLIST" 2>/dev/null || true
  launchctl bootstrap system "$PLIST" 2>/dev/null || launchctl load -w "$PLIST" 2>/dev/null || true
  FW="/usr/libexec/ApplicationFirewall/socketfilterfw"
  if [ -x "$FW" ]; then
    "$FW" --add "/usr/local/bin/lanvid_dsp_server" >/dev/null 2>&1 || true
    "$FW" --unblockapp "/usr/local/bin/lanvid_dsp_server" >/dev/null 2>&1 || true
  fi
fi
exit 0
DSPPOST
chmod 0755 "$SCRIPTS_DSP/postinstall"

# Component 3 (optional): com.lanvid.pkg.chmodbpf -> ChmodBPF helper + LaunchDaemon (/dev/bpf* via access_bpf)
ROOT_BPF="$BUILD_DIR/pkgroot-chmodbpf"
mkdir -p "$ROOT_BPF/Library/LaunchDaemons" "$ROOT_BPF/Library/Application Support/LanVid/ChmodBPF"
cat > "$ROOT_BPF/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" <<'BPFEOF'
#!/bin/bash
# LanVid Grid Layer 2 BPF permission helper (analogous to Wireshark ChmodBPF).
set -e
GROUP="access_bpf"
if ! dscl . -read "/Groups/$GROUP" >/dev/null 2>&1; then
  dseditgroup -q -o create "$GROUP" || true
fi
chgrp "$GROUP" /dev/bpf* 2>/dev/null || true
chmod g+rw /dev/bpf* 2>/dev/null || true
exit 0
BPFEOF
chmod 0755 "$ROOT_BPF/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"

cat > "$ROOT_BPF/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" <<'PLISTBPF'
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
chmod 0644 "$ROOT_BPF/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist"

SCRIPTS_BPF="$BUILD_DIR/scripts-chmodbpf"
mkdir -p "$SCRIPTS_BPF"
cat > "$SCRIPTS_BPF/postinstall" <<'BPFPOST'
#!/usr/bin/env bash
set -euo pipefail
CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then CONSOLE_USER="root"; fi
HELPER="/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"
PLIST="/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist"
if [ -x "$HELPER" ] && [ -f "$PLIST" ]; then
  if ! dscl . -read /Groups/access_bpf >/dev/null 2>&1; then
    dseditgroup -q -o create access_bpf || true
  fi
  if [ "$CONSOLE_USER" != "root" ]; then
    dseditgroup -q -o edit -a "$CONSOLE_USER" -t user access_bpf || true
  fi
  chown -R root:wheel "/Library/Application Support/LanVid/ChmodBPF" 2>/dev/null || true
  chmod 0755 "$HELPER" 2>/dev/null || true
  "$HELPER" || true
  chown root:wheel "$PLIST" 2>/dev/null || true
  chmod 0644 "$PLIST" 2>/dev/null || true
  launchctl bootout system "$PLIST" 2>/dev/null || launchctl unload -w "$PLIST" 2>/dev/null || true
  launchctl bootstrap system "$PLIST" 2>/dev/null || launchctl load -w "$PLIST" 2>/dev/null || true
fi
exit 0
BPFPOST
chmod 0755 "$SCRIPTS_BPF/postinstall"

# Component 4 (optional): com.lanvid.pkg.obsplugin -> OBS Studio plugin + locales
ROOT_OBS="$BUILD_DIR/pkgroot-obsplugin/usr/local/LanVidSDK/obs-lanvid"
mkdir -p "$ROOT_OBS/locale"
if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  cp "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$ROOT_OBS/locale/"
fi
for obs_cand in "$ROOT/build/plugins/obs-lanvid/obs-lanvid.plugin" "$ROOT/build-macos-app/plugins/obs-lanvid/obs-lanvid.plugin"; do
  if [ -d "$obs_cand" ]; then
    cp -R "$obs_cand" "$ROOT_OBS/"
    break
  fi
done
SCRIPTS_OBS="$BUILD_DIR/scripts-obsplugin"
mkdir -p "$SCRIPTS_OBS"
cat > "$SCRIPTS_OBS/postinstall" <<'OBSPOST'
#!/usr/bin/env bash
set -euo pipefail
CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then CONSOLE_USER="root"; fi
if [ "$CONSOLE_USER" != "root" ] && [ -d "/usr/local/LanVidSDK/obs-lanvid" ]; then
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
exit 0
OBSPOST
chmod 0755 "$SCRIPTS_OBS/postinstall"

# Component 5 & 6 (optional): Swift Package LanVidKit + Python/Examples
ROOT_SWIFT="$BUILD_DIR/pkgroot-swift/usr/local/LanVidSDK"
ROOT_EXTRAS="$BUILD_DIR/pkgroot-extras/usr/local/LanVidSDK"
mkdir -p "$ROOT_SWIFT/apple" "$ROOT_EXTRAS/python" "$ROOT_EXTRAS/examples"
cp -R "$APPLE_DIR/LanVidKit" "$ROOT_SWIFT/apple/"
sed 's|../../dist/apple/LanVidCore.xcframework|LanVidCore.xcframework|g' \
  "$APPLE_DIR/Package.swift" > "$ROOT_SWIFT/apple/Package.swift"
cp "$ROOT/sdk/python/lanvid_sdk.py" "$ROOT_EXTRAS/python/"
cp -R "$APPLE_DIR/Examples" "$ROOT_EXTRAS/apple-examples"
cp -R "$ROOT/sdk/examples/cpp_client" "$ROOT_EXTRAS/examples/"

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
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/LanVidAudio.driver" 2>/dev/null || true
killall -9 coreaudiod 2>/dev/null || true
exit 0
HALEOF
chmod 0755 "$HAL_SCRIPTS/postinstall"

# Virtual cameras (CoreMediaIO Camera Extension). Legacy DAL plug-ins are not loaded since macOS 14, so the
# cameras ship as a system extension inside "LanVid Virtual Camera.app"; it must be signed by a team.
VCAM_APP=""
if [ "${LANVID_SKIP_VCAM:-0}" != "1" ] && command -v xcodegen >/dev/null 2>&1; then
  if "$ROOT/drivers/macos_vcam/build_vcam.sh" && [ -d "$DIST_APPLE/LanVid Virtual Camera.app" ]; then
    VCAM_APP="$DIST_APPLE/LanVid Virtual Camera.app"
  else
    echo "[WARN] LanVid Virtual Camera.app not built (sign in to Xcode > Settings > Accounts); pkg without virtual cameras"
  fi
fi
if [ -n "$VCAM_APP" ]; then
  mkdir -p "$BUILD_DIR/pkgroot-vcam/Applications"
  cp -R "$VCAM_APP" "$BUILD_DIR/pkgroot-vcam/Applications/"
  VCAM_SCRIPTS="$BUILD_DIR/scripts-vcam"
  mkdir -p "$VCAM_SCRIPTS"
  cat > "$VCAM_SCRIPTS/postinstall" <<'VCAMEOF'
#!/bin/bash
# Ask macOS (in the logged-in user's session) to activate the camera extension.
CU="$(stat -f%Su /dev/console 2>/dev/null || true)"
if [ -n "$CU" ] && [ "$CU" != "root" ]; then
  launchctl asuser "$(id -u "$CU")" sudo -u "$CU" /usr/bin/open -a "/Applications/LanVid Virtual Camera.app" --args --install --quit-when-done || true
fi
exit 0
VCAMEOF
  chmod 0755 "$VCAM_SCRIPTS/postinstall"
fi

SCRIPTS_STAGE="$BUILD_DIR/scripts"
mkdir -p "$SCRIPTS_STAGE"
cp "$INST_DIR/scripts/preinstall" "$INST_DIR/scripts/postinstall" "$SCRIPTS_STAGE/"
chmod 0755 "$SCRIPTS_STAGE/preinstall" "$SCRIPTS_STAGE/postinstall"

echo "=== [5/7] Packaging modular component .pkgs and building LanVidSDK-v${SDK_VERSION}-macOS.pkg ==="
COMP_DIR="$BUILD_DIR/components"
mkdir -p "$COMP_DIR"

pkgbuild --root "$ROOT_STUDIO" --identifier "com.lanvid.pkg.studio" --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_STAGE" --install-location "/" "$COMP_DIR/lanvid-studio.pkg"

pkgbuild --root "$ROOT_AUDIO_APP" --identifier "com.lanvid.pkg.audiostudio" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-audiostudio.pkg"

pkgbuild --root "$ROOT_VIDEO_APP" --identifier "com.lanvid.pkg.videostudio" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-videostudio.pkg"

pkgbuild --root "$ROOT_DSP" --identifier "com.lanvid.pkg.dspserver" --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_DSP" --install-location "/" "$COMP_DIR/lanvid-dspserver.pkg"

pkgbuild --root "$ROOT_BPF" --identifier "com.lanvid.pkg.chmodbpf" --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_BPF" --install-location "/" "$COMP_DIR/lanvid-chmodbpf.pkg"

pkgbuild --root "$BUILD_DIR/pkgroot-obsplugin" --identifier "com.lanvid.pkg.obsplugin" --version "$SDK_VERSION" \
  --scripts "$SCRIPTS_OBS" --install-location "/" "$COMP_DIR/lanvid-obsplugin.pkg"

pkgbuild --root "$BUILD_DIR/pkgroot-hal" --identifier "com.lanvid.audio.hal" --version "$SDK_VERSION" \
  --scripts "$HAL_SCRIPTS" --install-location "/" "$COMP_DIR/lanvid-hal.pkg"

DIST_XML="$INST_DIR/distribution.xml"
if [ -n "$VCAM_APP" ]; then
  pkgbuild --root "$BUILD_DIR/pkgroot-vcam" --identifier "com.lanvid.pkg.vcam" --version "$SDK_VERSION" \
    --scripts "$VCAM_SCRIPTS" --install-location "/" "$COMP_DIR/lanvid-vcam.pkg"
  DIST_XML="$BUILD_DIR/distribution.xml"
  sed -e 's|        <line choice="choice_hal" />|        <line choice="choice_hal" />\
        <line choice="choice_vcam" />|' \
      -e 's|    <choice id="choice_swift"|    <choice id="choice_vcam" title="Virtual Cameras «LanVid Cam 1-8» + «LanVid Program Out»" description="CoreMediaIO Camera Extension: every phone camera from LanVid Video appears in FaceTime, Zoom, Teams, OBS and browsers. macOS asks once to allow it in System Settings." start_selected="true" enabled="true">\
        <pkg-ref id="com.lanvid.pkg.vcam" />\
    </choice>\
\
    <choice id="choice_swift"|' \
      -e 's|    <pkg-ref id="com.lanvid.sdk.swift" version|    <pkg-ref id="com.lanvid.pkg.vcam" version="'"$SDK_VERSION"'" onConclusion="none">lanvid-vcam.pkg</pkg-ref>\
    <pkg-ref id="com.lanvid.sdk.swift" version|' "$INST_DIR/distribution.xml" > "$DIST_XML"
fi

pkgbuild --root "$BUILD_DIR/pkgroot-swift" --identifier "com.lanvid.sdk.swift" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-swift.pkg"

pkgbuild --root "$BUILD_DIR/pkgroot-extras" --identifier "com.lanvid.sdk.extras" --version "$SDK_VERSION" \
  --install-location "/" "$COMP_DIR/lanvid-extras.pkg"

SIGN_ID="${LANVID_INSTALLER_SIGN_ID:-}"
if [ -z "$SIGN_ID" ] && command -v security >/dev/null 2>&1; then
  SIGN_ID="$(security find-identity -v -p basic 2>/dev/null | grep 'Developer ID Installer:' | head -n 1 | sed -E 's/.*"([^"]+)".*/\1/' || true)"
fi

PROD_ARGS=(
  --distribution "$DIST_XML"
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
