#!/usr/bin/env bash
# Builds the LanVid Monitor iOS release artifacts into dist/ios (unsigned - re-sign with your own team):
#   LanVid-Monitor-v<ver>-iOS.zip              the .xcarchive (open in Xcode Organizer -> Distribute App)
#   LanVid-Monitor-v<ver>-iOS-unsigned.ipa     Payload/LanVid Monitor.app (sideload / re-sign tools)
#   LanVidCore.xcframework.zip                 core C ABI SDK (iPhone, Simulator, macOS) with libopus folded in
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(sed -n 's/^project(lanvid VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
[ -n "$VER" ] || { echo "Cannot read the project version from CMakeLists.txt" >&2; exit 1; }
OUT="$ROOT/dist/ios"
ARCHIVE="$HERE/build/LanVidMonitor.xcarchive"
export COPYFILE_DISABLE=1

rm -rf "$OUT" "$ARCHIVE"
mkdir -p "$OUT"

"$HERE/scripts/build_core.sh"
(cd "$HERE" && xcodegen generate)
xcodebuild -project "$HERE/LanVidMonitor.xcodeproj" -scheme LanVidMonitor -configuration Release \
  -destination 'generic/platform=iOS' -archivePath "$ARCHIVE" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" archive

APP="$ARCHIVE/Products/Applications/LanVid Monitor.app"
[ -d "$APP" ] || { echo "Archive has no LanVid Monitor.app" >&2; exit 1; }
SHORT="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Info.plist")"
[ "$SHORT" = "$VER" ] || { echo "App version $SHORT != project version $VER" >&2; exit 1; }

ditto -c -k --sequesterRsrc --keepParent "$ARCHIVE" "$OUT/LanVid-Monitor-v${VER}-iOS.zip"
TMP="$(mktemp -d)"
mkdir -p "$TMP/Payload"
cp -R "$APP" "$TMP/Payload/"
(cd "$TMP" && zip -qry "$OUT/LanVid-Monitor-v${VER}-iOS-unsigned.ipa" Payload)
rm -rf "$TMP"

# SDK xcframework (libopus included; see sdk/apple/build_xcframework.sh)
"$ROOT/sdk/apple/build_xcframework.sh"
ditto -c -k --sequesterRsrc --keepParent "$ROOT/dist/apple/LanVidCore.xcframework" "$OUT/LanVidCore.xcframework.zip"
ls -lh "$OUT"
