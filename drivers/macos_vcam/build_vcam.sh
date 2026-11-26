#!/usr/bin/env bash
# Builds "LanVid Virtual Camera.app" (+ embedded CoreMediaIO Camera Extension) into dist/apple/.
#   TEAM_ID=XXXXXXXXXX ./drivers/macos_vcam/build_vcam.sh [--install]
# --install copies the app to /Applications and asks macOS to activate the extension
# (the user approves it once in System Settings -> General -> Login Items & Extensions -> Camera Extensions).
# The host LanVid Video must publish to the same shared memory name: CMake option
# -DLANVID_MACOS_APP_GROUP=<TEAM_ID>.lanvid (default 9SN7V4SC2S.lanvid).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export TEAM_ID="${TEAM_ID:-9SN7V4SC2S}"
export LANVID_APP_GROUP="${LANVID_APP_GROUP:-$TEAM_ID.lanvid}"
OUT="$ROOT/dist/apple"
DERIVED="$ROOT/build-vcam"
command -v xcodegen >/dev/null || { echo "[ERROR] xcodegen not found (brew install xcodegen)" >&2; exit 1; }
cd "$HERE"
xcodegen generate --quiet
xcodebuild -project LanVidVirtualCamera.xcodeproj -scheme LanVidVirtualCamera -configuration Release \
  -derivedDataPath "$DERIVED" -allowProvisioningUpdates build | grep -E "error|warning: |BUILD" || true
APP="$DERIVED/Build/Products/Release/LanVid Virtual Camera.app"
[ -d "$APP" ] || { echo "[ERROR] build failed" >&2; exit 1; }
mkdir -p "$OUT"
rm -rf "$OUT/LanVid Virtual Camera.app"
cp -R "$APP" "$OUT/"
echo "Built: $OUT/LanVid Virtual Camera.app"
if [ "${1:-}" = "--install" ]; then
  rm -rf "/Applications/LanVid Virtual Camera.app"
  cp -R "$APP" /Applications/
  open -a "/Applications/LanVid Virtual Camera.app" --args --install
  echo "Activation requested: approve 'LanVid Virtual Camera' in System Settings if macOS asks."
fi
