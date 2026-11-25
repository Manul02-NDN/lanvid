#!/usr/bin/env bash
# Builds the customizable all-in-one LanVid Studio + Grid L2 + DSP Server macOS installer:
#   dist/apple/LanVid-Studio-v<ver>-macOS.pkg  (Standard Install = all components; Customize = toggle optional modules)
#     - com.lanvid.pkg.studio     (mandatory, enabled="false"): LanVid Studio.app + liblanvid_core.dylib + Firewall
#     - com.lanvid.pkg.dspserver  (optional): /usr/local/bin/lanvid_dsp_server + com.lanvid.dspserver.plist
#     - com.lanvid.pkg.chmodbpf   (optional): ChmodBPF + com.lanvid.chmod-bpf.plist (access_bpf -> /dev/bpf*)
#     - com.lanvid.pkg.obsplugin  (optional): obs-lanvid.plugin -> ~/Library/Application Support/obs-studio/plugins/
#     - com.lanvid.studio.hal     (optional): LanVidAudio.driver (64ch CoreAudio HAL)
#   dist/apple/LanVid-Studio-v<ver>-macOS.dmg  drag-to-Applications disk image carrying the .pkg
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
export COPYFILE_DISABLE=1
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

echo "=== Packaging modular components (Standard Install + Customize) ==="
mkdir -p "$STAGE/studio/Applications" "$STAGE/studio/usr/local/lib" "$STAGE/scripts-studio" \
         "$STAGE/dsp/usr/local/bin" "$STAGE/dsp/Library/LaunchDaemons" "$STAGE/scripts-dsp" \
         "$STAGE/bpf/Library/LaunchDaemons" "$STAGE/bpf/Library/Application Support/LanVid/ChmodBPF" "$STAGE/scripts-bpf" \
         "$STAGE/obs/Library/Application Support/LanVid/obs-lanvid/locale" "$STAGE/scripts-obs" \
         "$STAGE/hal/Library/Audio/Plug-Ins/HAL" "$STAGE/scripts-hal" \
         "$STAGE/pkgs"

cp -R "$APP" "$STAGE/studio/Applications/"
cp "$DYLIB" "$STAGE/studio/usr/local/lib/liblanvid_core.dylib"
cp "$DSP" "$STAGE/dsp/usr/local/bin/lanvid_dsp_server"
chmod 0755 "$STAGE/dsp/usr/local/bin/lanvid_dsp_server"
cp -R "$HAL" "$STAGE/hal/Library/Audio/Plug-Ins/HAL/"

if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  cp "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$STAGE/obs/Library/Application Support/LanVid/obs-lanvid/locale/"
fi
if [ -d "$BUILD/cmake/plugins/obs-lanvid/obs-lanvid.plugin" ]; then
  cp -R "$BUILD/cmake/plugins/obs-lanvid/obs-lanvid.plugin" "$STAGE/obs/Library/Application Support/LanVid/obs-lanvid/"
fi

# Studio postinstall (Firewall for Studio & Core)
cat > "$STAGE/scripts-studio/postinstall" <<'STPOST'
#!/usr/bin/env bash
set -euo pipefail
FW="/usr/libexec/ApplicationFirewall/socketfilterfw"
if [ -x "$FW" ]; then
  for target in "/Applications/LanVid Studio.app" "/Applications/LanVid Studio.app/Contents/MacOS/LanVid Studio" "/usr/local/lib/liblanvid_core.dylib"; do
    if [ -e "$target" ]; then
      "$FW" --add "$target" >/dev/null 2>&1 || true
      "$FW" --unblockapp "$target" >/dev/null 2>&1 || true
    fi
  done
fi
exit 0
STPOST
chmod 0755 "$STAGE/scripts-studio/postinstall"

# DSP Server LaunchDaemon + postinstall
cat > "$STAGE/dsp/Library/LaunchDaemons/com.lanvid.dspserver.plist" <<'PLISTDSP'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.lanvid.dspserver</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/lanvid_dsp_server</string>
    <string>--name</string>
    <string>LanVid Grid DSP</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>/var/log/lanvid-dsp.log</string>
  <key>StandardErrorPath</key><string>/var/log/lanvid-dsp.log</string>
</dict>
</plist>
PLISTDSP
chmod 0644 "$STAGE/dsp/Library/LaunchDaemons/com.lanvid.dspserver.plist"
cat > "$STAGE/scripts-dsp/postinstall" <<'DSPPOST'
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
chmod 0755 "$STAGE/scripts-dsp/postinstall"

# ChmodBPF helper + LaunchDaemon + postinstall
cat > "$STAGE/bpf/Library/Application Support/LanVid/ChmodBPF/ChmodBPF" <<'BPFEOF'
#!/bin/bash
set -e
GROUP="access_bpf"
if ! dscl . -read "/Groups/$GROUP" >/dev/null 2>&1; then dseditgroup -q -o create "$GROUP" || true; fi
chgrp "$GROUP" /dev/bpf* 2>/dev/null || true
chmod g+rw /dev/bpf* 2>/dev/null || true
exit 0
BPFEOF
chmod 0755 "$STAGE/bpf/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"
cat > "$STAGE/bpf/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist" <<'PLISTBPF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.lanvid.chmod-bpf</string>
  <key>ProgramArguments</key><array><string>/Library/Application Support/LanVid/ChmodBPF/ChmodBPF</string></array>
  <key>RunAtLoad</key><true/>
</dict>
</plist>
PLISTBPF
chmod 0644 "$STAGE/bpf/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist"
cat > "$STAGE/scripts-bpf/postinstall" <<'BPFPOST'
#!/usr/bin/env bash
set -euo pipefail
CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then CONSOLE_USER="root"; fi
HELPER="/Library/Application Support/LanVid/ChmodBPF/ChmodBPF"
PLIST="/Library/LaunchDaemons/com.lanvid.chmod-bpf.plist"
if [ -x "$HELPER" ] && [ -f "$PLIST" ]; then
  if ! dscl . -read /Groups/access_bpf >/dev/null 2>&1; then dseditgroup -q -o create access_bpf || true; fi
  if [ "$CONSOLE_USER" != "root" ]; then dseditgroup -q -o edit -a "$CONSOLE_USER" -t user access_bpf || true; fi
  chown -R root:wheel "/Library/Application Support/LanVid/ChmodBPF" 2>/dev/null || true
  "$HELPER" || true
  chown root:wheel "$PLIST" 2>/dev/null || true
  chmod 0644 "$PLIST" 2>/dev/null || true
  launchctl bootout system "$PLIST" 2>/dev/null || launchctl unload -w "$PLIST" 2>/dev/null || true
  launchctl bootstrap system "$PLIST" 2>/dev/null || launchctl load -w "$PLIST" 2>/dev/null || true
fi
exit 0
BPFPOST
chmod 0755 "$STAGE/scripts-bpf/postinstall"

# OBS Plugin postinstall
cat > "$STAGE/scripts-obs/postinstall" <<'OBSPOST'
#!/usr/bin/env bash
set -euo pipefail
CONSOLE_USER="$(stat -f "%Su" /dev/console 2>/dev/null || echo "${USER:-root}")"
if [ "$CONSOLE_USER" = "loginwindow" ] || [ -z "$CONSOLE_USER" ]; then CONSOLE_USER="root"; fi
if [ "$CONSOLE_USER" != "root" ] && [ -d "/Library/Application Support/LanVid/obs-lanvid" ]; then
  USER_HOME="$(dscl . -read "/Users/$CONSOLE_USER" NFSHomeDirectory 2>/dev/null | awk '{print $2}' || echo "/Users/$CONSOLE_USER")"
  OBS_USER_DIR="$USER_HOME/Library/Application Support/obs-studio"
  if [ -d "/Applications/OBS.app" ] || [ -d "$OBS_USER_DIR" ]; then
    mkdir -p "$OBS_USER_DIR/plugins"
    if [ -d "/Library/Application Support/LanVid/obs-lanvid/obs-lanvid.plugin" ]; then
      rm -rf "$OBS_USER_DIR/plugins/obs-lanvid.plugin"
      cp -R "/Library/Application Support/LanVid/obs-lanvid/obs-lanvid.plugin" "$OBS_USER_DIR/plugins/"
    fi
    chown -R "$CONSOLE_USER":staff "$OBS_USER_DIR" 2>/dev/null || true
  fi
fi
exit 0
OBSPOST
chmod 0755 "$STAGE/scripts-obs/postinstall"

# CoreAudio HAL postinstall
cat > "$STAGE/scripts-hal/postinstall" <<'HAL'
#!/bin/sh
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/LanVidAudio.driver" 2>/dev/null || true
killall -9 coreaudiod 2>/dev/null || true
exit 0
HAL
chmod 0755 "$STAGE/scripts-hal/postinstall"

pkgbuild --root "$STAGE/studio" --identifier com.lanvid.pkg.studio --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-studio" "$STAGE/pkgs/lanvid-studio.pkg"
pkgbuild --root "$STAGE/dsp" --identifier com.lanvid.pkg.dspserver --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-dsp" "$STAGE/pkgs/lanvid-dspserver.pkg"
pkgbuild --root "$STAGE/bpf" --identifier com.lanvid.pkg.chmodbpf --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-bpf" "$STAGE/pkgs/lanvid-chmodbpf.pkg"
pkgbuild --root "$STAGE/obs" --identifier com.lanvid.pkg.obsplugin --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-obs" "$STAGE/pkgs/lanvid-obsplugin.pkg"
pkgbuild --root "$STAGE/hal" --identifier com.lanvid.studio.hal --version "$VER" \
         --install-location / --scripts "$STAGE/scripts-hal" "$STAGE/pkgs/lanvid-hal.pkg"

cat > "$STAGE/distribution.xml" <<DISTXML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>LanVid Studio + Grid L2 v${VER}</title>
  <organization>com.lanvid</organization>
  <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false" />
  <options customize="allow" require-scripts="true" rootVolumeOnly="true" hostArchitectures="arm64,x86_64" />
  <choices-outline>
    <line choice="choice_studio" />
    <line choice="choice_dspserver" />
    <line choice="choice_chmodbpf" />
    <line choice="choice_obsplugin" />
    <line choice="choice_hal" />
  </choices-outline>
  <choice id="choice_studio" title="LanVid Studio.app (Обов'язково)" start_selected="true" enabled="false"><pkg-ref id="com.lanvid.pkg.studio" /></choice>
  <choice id="choice_dspserver" title="LanVid Grid DSP Server (LaunchDaemon)" start_selected="true" enabled="true"><pkg-ref id="com.lanvid.pkg.dspserver" /></choice>
  <choice id="choice_chmodbpf" title="LanVid Grid L2 — ChmodBPF (/dev/bpf*)" start_selected="true" enabled="true"><pkg-ref id="com.lanvid.pkg.chmodbpf" /></choice>
  <choice id="choice_obsplugin" title="OBS Studio Plugin (obs-lanvid.plugin)" start_selected="true" enabled="true"><pkg-ref id="com.lanvid.pkg.obsplugin" /></choice>
  <choice id="choice_hal" title="Virtual Audio «LanVid Virtual 64ch»" start_selected="true" enabled="true"><pkg-ref id="com.lanvid.studio.hal" /></choice>
  <pkg-ref id="com.lanvid.pkg.studio" version="${VER}">lanvid-studio.pkg</pkg-ref>
  <pkg-ref id="com.lanvid.pkg.dspserver" version="${VER}">lanvid-dspserver.pkg</pkg-ref>
  <pkg-ref id="com.lanvid.pkg.chmodbpf" version="${VER}">lanvid-chmodbpf.pkg</pkg-ref>
  <pkg-ref id="com.lanvid.pkg.obsplugin" version="${VER}">lanvid-obsplugin.pkg</pkg-ref>
  <pkg-ref id="com.lanvid.studio.hal" version="${VER}">lanvid-hal.pkg</pkg-ref>
</installer-gui-script>
DISTXML

if [ -n "${LANVID_INSTALLER_SIGN_ID:-}" ]; then
  productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" --version "$VER" \
               --sign "$LANVID_INSTALLER_SIGN_ID" "$PKG"
else
  productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/pkgs" --version "$VER" "$PKG"
fi

DMG_SRC="$STAGE/dmg"
mkdir -p "$DMG_SRC"
cp -R "$APP" "$DMG_SRC/"
ln -s /Applications "$DMG_SRC/Applications"
cp "$PKG" "$DMG_SRC/Install LanVid Studio + Grid L2 + Virtual Audio.pkg"
hdiutil create -volname "LanVid Studio $VER" -srcfolder "$DMG_SRC" -ov -format UDZO "$DMG"

echo "Built $PKG"
echo "Built $DMG"
