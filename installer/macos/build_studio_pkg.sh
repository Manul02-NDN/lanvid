#!/usr/bin/env bash
# Builds the all-in-one LanVid Studio + Grid L2 + DSP Server macOS installer:
#   dist/apple/LanVid-Studio-v<ver>-macOS.pkg
#     - LanVid Studio.app                -> /Applications/
#     - lanvid_dsp_server                -> /usr/local/bin/lanvid_dsp_server
#     - liblanvid_core.dylib             -> /usr/local/lib/liblanvid_core.dylib
#     - LanVidAudio.driver (64ch HAL)    -> /Library/Audio/Plug-Ins/HAL/ (coreaudiod restarted)
#     - com.lanvid.dspserver.plist       -> /Library/LaunchDaemons/ (KeepAlive + RunAtLoad)
#     - com.lanvid.chmod-bpf.plist       -> /Library/LaunchDaemons/ (rootless /dev/bpf* via access_bpf)
#     - obs-lanvid.plugin (auto-copied to ~/Library/Application Support/obs-studio/plugins/ if OBS installed)
#     - macOS Application Firewall rules via socketfilterfw (--add & --unblockapp)
#   dist/apple/LanVid-Studio-v<ver>-macOS.dmg   drag-to-Applications disk image that also carries the .pkg
#
# Run on macOS with Xcode Command Line Tools + CMake + SDL2:   ./installer/macos/build_studio_pkg.sh
# Optional signing: LANVID_APP_SIGN_ID="Developer ID Application: ..." LANVID_INSTALLER_SIGN_ID="Developer ID Installer: ..."
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VER="$(sed -n 's/^project(lanvid VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
[ -n "$VER" ] || { echo "Cannot read the project version from CMakeLists.txt" >&2; exit 1; }
BUILD="$ROOT/build-studio-pkg"
STAGE="$BUILD/stage"
DIST="$ROOT/dist/apple"
PKG="$DIST/LanVid-Studio-v${VER}-macOS.pkg"
DMG="$DIST/LanVid-Studio-v${VER}-macOS.dmg"
GEN="Unix Makefiles"; command -v ninja >/dev/null 2>&1 && GEN="Ninja"

[ "$(uname -s)" = "Darwin" ] || { echo "Run on macOS (pkgbuild, productbuild, hdiutil)." >&2; exit 1; }
export COPYFILE_DISABLE=1  # no AppleDouble (._*) files inside the packages
rm -rf "$BUILD" "$PKG" "$DMG"
mkdir -p "$STAGE" "$DIST"

echo "=== Building LanVid Studio.app + lanvid_dsp_server + liblanvid_core.dylib + LanVidAudio.driver ($VER) ==="
cmake -S "$ROOT" -B "$BUILD/cmake" -G "$GEN" -DCMAKE_BUILD_TYPE=Release \
  -DLANVID_BUILD_TESTS=OFF -DLANVID_BUILD_DRIVERS=ON -DLANVID_BUILD_CLI=ON -DLANVID_BUILD_SHARED=ON
cmake --build "$BUILD/cmake" --target lanvid_app lanvid_dsp_server lanvid_core_shared LanVidAudio --parallel "$(sysctl -n hw.ncpu)"
APP="$BUILD/cmake/LanVid Studio.app"
DSP="$BUILD/cmake/lanvid_dsp_server"
DYLIB="$BUILD/cmake/liblanvid_core.dylib"
HAL="$BUILD/cmake/LanVidAudio.driver"
[ -d "$APP" ] || { echo "LanVid Studio.app missing" >&2; exit 1; }
[ -x "$DSP" ] || { echo "lanvid_dsp_server missing" >&2; exit 1; }
[ -f "$DYLIB" ] || { echo "liblanvid_core.dylib missing" >&2; exit 1; }
[ -f "$HAL/Contents/MacOS/LanVidAudio" ] || { echo "LanVidAudio.driver missing" >&2; exit 1; }

install_name_tool -id "/usr/local/lib/liblanvid_core.dylib" "$DYLIB"
if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
  codesign --force --deep --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$APP"
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$DSP"
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$DYLIB"
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$HAL"
else
  codesign --force --deep --sign - "$APP"
  codesign --force --sign - "$DSP"
  codesign --force --sign - "$DYLIB"
  codesign --force --sign - "$HAL"
fi
/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist" | grep -qx "$VER" \
  || { echo "App bundle version does not match $VER" >&2; exit 1; }

echo "=== Packaging ==="
mkdir -p "$STAGE/app/Applications" \
         "$STAGE/app/usr/local/bin" \
         "$STAGE/app/usr/local/lib" \
         "$STAGE/app/Library/LaunchDaemons" \
         "$STAGE/app/Library/Application Support/LanVid/ChmodBPF" \
         "$STAGE/app/Library/Application Support/LanVid/obs-lanvid" \
         "$STAGE/hal/Library/Audio/Plug-Ins/HAL" \
         "$STAGE/scripts-app" \
         "$STAGE/scripts-hal"

cp -R "$APP" "$STAGE/app/Applications/"
cp "$DSP" "$STAGE/app/usr/local/bin/lanvid_dsp_server"
chmod 0755 "$STAGE/app/usr/local/bin/lanvid_dsp_server"
cp "$DYLIB" "$STAGE/app/usr/local/lib/liblanvid_core.dylib"
cp -R "$HAL" "$STAGE/hal/Library/Audio/Plug-Ins/HAL/"

if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  mkdir -p "$STAGE/app/Library/Application Support/LanVid/obs-lanvid/locale"
  cp "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$STAGE/app/Library/Application Support/LanVid/obs-lanvid/locale/"
fi
if [ -d "$BUILD/cmake/plugins/obs-lanvid/obs-lanvid.plugin" ]; then
  cp -R "$BUILD/cmake/plugins/obs-lanvid/obs-lanvid.plugin" "$STAGE/app/Library/Application Support/LanVid/obs-lanvid/"
fi

# ChmodBPF helper for rootless /dev/bpf* access (LanVid Grid L2)
cat > "$STAGE/app/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" <<'BPFEOF'
#!/bin/bash
set -e
GROUP="access_bpf"
if ! dscl . -read "/Groups/$GROUP" >/dev/null 2>&1; then
  dseditgroup -q -o create "$GROUP" || true
fi
chgrp "$GROUP" /dev/bpf* 2>/dev/null || true
chmod g+rw /dev/bpf* 2>/dev/null || true
exit 0
BPFEOF
chmod 0755 "$STAGE/app/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"

cat > "$STAGE/app/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" <<'PLISTBPF'
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

cat > "$STAGE/app/Library/LaunchDaemons/com.lanvid.dspserver.plist" <<'PLISTDSP'
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
chmod 0644 "$STAGE/app/Library/LaunchDaemons/"*.plist

cat > "$STAGE/scripts-app/postinstall" <<'APPPOST'
#!/usr/bin/env bash
set -euo pipefail

CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then
  CONSOLE_USER="root"
fi

# 1. Layer 2 ChmodBPF group & permissions
if ! dscl . -read /Groups/access_bpf >/dev/null 2>&1; then
  dseditgroup -q -o create access_bpf || true
fi
if [ "$CONSOLE_USER" != "root" ]; then
  dseditgroup -q -o edit -a "$CONSOLE_USER" -t user access_bpf || true
fi
chown -R root:wheel "/Library/Application Support/LanVid/ChmodBPF" 2>/dev/null || true
chmod 0755 "/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" 2>/dev/null || true
"/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" || true

# 2. Load LaunchDaemons (ChmodBPF + LanVid Grid DSP Server)
for plist in "/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" "/Library/LaunchDaemons/com.lanvid.dspserver.plist"; do
  if [ -f "$plist" ]; then
    chown root:wheel "$plist" 2>/dev/null || true
    chmod 0644 "$plist" 2>/dev/null || true
    launchctl bootout system "$plist" 2>/dev/null || launchctl unload -w "$plist" 2>/dev/null || true
    launchctl bootstrap system "$plist" 2>/dev/null || launchctl load -w "$plist" 2>/dev/null || true
  fi
done

# 3. Auto-install OBS Studio plugin when OBS is detected
if [ "$CONSOLE_USER" != "root" ]; then
  USER_HOME="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}' || echo "/Users/$CONSOLE_USER")"
  OBS_USER_DIR="$USER_HOME/Library/Application Support/obs-studio"
  if [ -d "/Applications/OBS.app" ] || [ -d "$OBS_USER_DIR" ]; then
    OBS_PLUGIN_DST="$OBS_USER_DIR/plugins"
    mkdir -p "$OBS_PLUGIN_DST"
    if [ -d "/Library/Application Support/LanVid/obs-lanvid/obs-lanvid.plugin" ]; then
      rm -rf "$OBS_PLUGIN_DST/obs-lanvid.plugin"
      cp -R "/Library/Application Support/LanVid/obs-lanvid/obs-lanvid.plugin" "$OBS_PLUGIN_DST/"
    fi
    chown -R "$CONSOLE_USER":staff "$OBS_USER_DIR" 2>/dev/null || true
  fi
fi

# 4. macOS Application Firewall exemptions
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
APPPOST
chmod 0755 "$STAGE/scripts-app/postinstall"

cat > "$STAGE/scripts-hal/postinstall" <<'HAL'
#!/bin/sh
# Reload the HAL so "LanVid Virtual 64ch" appears in Audio MIDI Setup / DAWs (launchd restarts coreaudiod).
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/LanVidAudio.driver" 2>/dev/null || true
killall -9 coreaudiod 2>/dev/null || true
exit 0
HAL
chmod 0755 "$STAGE/scripts-hal/postinstall"

pkgbuild --root "$STAGE/app" --identifier com.lanvid.studio.app --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-app" "$STAGE/studio-app.pkg"
pkgbuild --root "$STAGE/hal" --identifier com.lanvid.studio.hal --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-hal" "$STAGE/studio-hal.pkg"
if [ -n "${LANVID_INSTALLER_SIGN_ID:-}" ]; then
  productbuild --package "$STAGE/studio-app.pkg" --package "$STAGE/studio-hal.pkg" \
               --sign "$LANVID_INSTALLER_SIGN_ID" "$PKG"
else
  productbuild --package "$STAGE/studio-app.pkg" --package "$STAGE/studio-hal.pkg" "$PKG"
fi

DMG_SRC="$STAGE/dmg"
mkdir -p "$DMG_SRC"
cp -R "$APP" "$DMG_SRC/"
ln -s /Applications "$DMG_SRC/Applications"
cp "$PKG" "$DMG_SRC/Install LanVid Studio + Grid L2 + Virtual Audio.pkg"
hdiutil create -volname "LanVid Studio $VER" -srcfolder "$DMG_SRC" -ov -format UDZO "$DMG"

echo "Built $PKG"
echo "Built $DMG"
