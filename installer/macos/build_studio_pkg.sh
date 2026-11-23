#!/usr/bin/env bash
# Builds the LanVid Studio macOS installer:
#   dist/apple/LanVid-Studio-v<ver>-macOS.pkg   LanVid Studio.app -> /Applications
#                                               + LanVidAudio.driver (CoreAudio HAL, virtual 64ch device)
#                                                 -> /Library/Audio/Plug-Ins/HAL (coreaudiod restarted)
#   dist/apple/LanVid-Studio-v<ver>-macOS.dmg   drag-to-Applications disk image that also carries the .pkg
# CoreMIDI needs no installer component: Studio publishes its virtual ports ("LanVid Virtual MIDI In/Out",
# "LanVid MIDI In/Out") through CoreMIDI at run time.
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

echo "=== Building LanVid Studio.app + LanVidAudio.driver ($VER) ==="
cmake -S "$ROOT" -B "$BUILD/cmake" -G "$GEN" -DCMAKE_BUILD_TYPE=Release \
  -DLANVID_BUILD_TESTS=OFF -DLANVID_BUILD_DRIVERS=ON -DLANVID_BUILD_CLI=OFF
cmake --build "$BUILD/cmake" --target lanvid_app LanVidAudio --parallel "$(sysctl -n hw.ncpu)"
APP="$BUILD/cmake/LanVid Studio.app"
HAL="$BUILD/cmake/LanVidAudio.driver"
[ -d "$APP" ] || { echo "LanVid Studio.app missing" >&2; exit 1; }
[ -f "$HAL/Contents/MacOS/LanVidAudio" ] || { echo "LanVidAudio.driver missing" >&2; exit 1; }

if [ -n "${LANVID_APP_SIGN_ID:-}" ]; then
  codesign --force --deep --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$APP"
  codesign --force --options runtime --timestamp --sign "$LANVID_APP_SIGN_ID" "$HAL"
else
  codesign --force --deep --sign - "$APP"
  codesign --force --sign - "$HAL"
fi
/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist" | grep -qx "$VER" \
  || { echo "App bundle version does not match $VER" >&2; exit 1; }

echo "=== Packaging ==="
mkdir -p "$STAGE/app/Applications" "$STAGE/hal/Library/Audio/Plug-Ins/HAL" "$STAGE/scripts-hal"
cp -R "$APP" "$STAGE/app/Applications/"
cp -R "$HAL" "$STAGE/hal/Library/Audio/Plug-Ins/HAL/"
cat > "$STAGE/scripts-hal/postinstall" <<'HAL'
#!/bin/sh
# Reload the HAL so "LanVid Virtual 64ch" appears in Audio MIDI Setup / DAWs (launchd restarts coreaudiod).
chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/LanVidAudio.driver" 2>/dev/null || true
killall -9 coreaudiod 2>/dev/null || true
exit 0
HAL
chmod 0755 "$STAGE/scripts-hal/postinstall"

pkgbuild --root "$STAGE/app" --identifier com.lanvid.studio.app --version "$VER" \
         --install-location / "$STAGE/studio-app.pkg"
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
cp "$PKG" "$DMG_SRC/Install LanVid Studio + Virtual Audio.pkg"
hdiutil create -volname "LanVid Studio $VER" -srcfolder "$DMG_SRC" -ov -format UDZO "$DMG"

echo "Built $PKG"
echo "Built $DMG"
