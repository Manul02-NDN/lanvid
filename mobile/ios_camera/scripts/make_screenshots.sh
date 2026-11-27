#!/usr/bin/env bash
# Captures 4 marketing App Store screenshots from LanVid Camera in the iOS Simulator (Demo Cine Studio)
# in both English (en-US) and Ukrainian (uk), at 1320x2868 (6.9") and 1290x2796 (6.7") RGB PNG.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="${1:-iPhone 18 Pro Max}"
if ! xcrun simctl list devices available | grep -qF "$DEVICE ("; then
  DEVICE="$(xcrun simctl list devices available | grep -E "iPhone [0-9]+ Pro Max \(" | tail -n 1 | sed -E 's/^ *(.*) \([0-9A-F-]{36}\).*/\1/')"
  [ -n "$DEVICE" ] || { echo "No Pro Max iPhone simulator available" >&2; exit 1; }
  echo "Using simulator: $DEVICE"
fi

RAW="$HERE/build/screenshots-raw"
BUNDLE="com.manul02.lanvid.camera"
cd "$HERE"

[ -d build/LanVidCore.xcframework ] || ./scripts/build_core.sh
xcodegen generate >/dev/null
xcodebuild -project LanVidCamera.xcodeproj -scheme LanVidCamera -sdk iphonesimulator -configuration Debug \
  -destination "platform=iOS Simulator,name=$DEVICE" -derivedDataPath build/DerivedData \
  CODE_SIGNING_ALLOWED=NO build >/dev/null
APP="$(find build/DerivedData/Build/Products -name 'LanVid Camera.app' -path '*iphonesimulator*' | head -n 1)"

UDID="$(xcrun simctl list devices available | grep -F "$DEVICE (" | head -n 1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/')"
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl status_bar "$UDID" override --time "9:41" --batteryState charged --batteryLevel 100 \
  --cellularMode active --cellularBars 4 --wifiBars 3 2>/dev/null || true
xcrun simctl ui "$UDID" appearance dark 2>/dev/null || true
xcrun simctl install "$UDID" "$APP"

rm -rf "$RAW"

has_content() {
  python3 - "$1" <<'PY'
import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]).convert("RGB")
body = im.crop((0, int(im.height * 0.12), im.width, int(im.height * 0.9)))
sys.exit(0 if (sum(ImageStat.Stat(body).mean) / 3) > 8 else 1)
PY
}

capture() {
  local lang="$1" shot="$2" out="$3" attempt i
  for attempt in 1 2 3; do
    xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
    xcrun simctl launch "$UDID" "$BUNDLE" -LVLang "$lang" -LVDemo -LVShot "$shot" >/dev/null
    for i in $(seq 1 15); do
      sleep 1.5
      xcrun simctl io "$UDID" screenshot "$out" >/dev/null 2>&1 || continue
      if has_content "$out"; then
        sleep 1.0
        xcrun simctl io "$UDID" screenshot "$out" >/dev/null
        echo "captured $lang shot $shot (attempt $attempt)"
        return 0
      fi
    done
  done
  echo "FAILED to capture $lang shot $shot" >&2
  return 1
}

for lang in en uk; do
  mkdir -p "$RAW/$lang"
  for shot in 1 2 3 4; do
    capture "$lang" "$shot" "$RAW/$lang/${shot}.png"
  done
done

xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl status_bar "$UDID" clear 2>/dev/null || true

python3 "$HERE/scripts/compose_screenshots.py" "$RAW" "$HERE/fastlane/screenshots"
