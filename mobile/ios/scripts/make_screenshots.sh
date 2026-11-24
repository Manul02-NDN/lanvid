#!/usr/bin/env bash
# Regenerates the App Store screenshots from the real app running in the iOS Simulator (Demo Studio):
#   mobile/ios/fastlane/screenshots/{en-US,uk}/iPhone69_*.png (1320x2868) and iPhone67_*.png (1290x2796)
# Needs Xcode with an iOS Simulator runtime (xcodebuild -downloadPlatform iOS), XcodeGen, Python 3 + Pillow.
#   ./mobile/ios/scripts/make_screenshots.sh [simulator name]      default: iPhone 17 Pro Max
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="${1:-iPhone 17 Pro Max}"
if ! xcrun simctl list devices available | grep -qF "$DEVICE ("; then
  # fall back to any 6.9" class "Pro Max" simulator of the installed runtime
  DEVICE="$(xcrun simctl list devices available | grep -E "iPhone [0-9]+ Pro Max \(" | tail -n 1 | sed -E 's/^ *(.*) \([0-9A-F-]{36}\).*/\1/')"
  [ -n "$DEVICE" ] || { echo "No Pro Max iPhone simulator available (install a runtime: xcodebuild -downloadPlatform iOS)" >&2; exit 1; }
  echo "Using simulator: $DEVICE"
fi
RAW="$HERE/build/screenshots-raw"
BUNDLE="com.manul02.lanvid.monitor"
cd "$HERE"

[ -d build/LanVidCore.xcframework ] || ./scripts/build_core.sh
xcodegen generate >/dev/null
xcodebuild -project LanVidMonitor.xcodeproj -scheme LanVidMonitor -sdk iphonesimulator -configuration Debug \
  -destination "platform=iOS Simulator,name=$DEVICE" -derivedDataPath build/DerivedData \
  CODE_SIGNING_ALLOWED=NO build >/dev/null
APP="$(find build/DerivedData/Build/Products -name 'LanVid Monitor.app' -path '*iphonesimulator*' | head -n 1)"

UDID="$(xcrun simctl list devices available | grep -F "$DEVICE (" | head -n 1 | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/')"
[ -n "$UDID" ] || { echo "Simulator '$DEVICE' not found" >&2; exit 1; }
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl status_bar "$UDID" override --time "9:41" --batteryState charged --batteryLevel 100 \
  --cellularMode active --cellularBars 4 --wifiBars 3 2>/dev/null || true
xcrun simctl ui "$UDID" appearance dark 2>/dev/null || true
xcrun simctl install "$UDID" "$APP"
# The Local Network prompt would cover the UI: demo / screenshot runs never browse (see StudioBrowser).

rm -rf "$RAW"

# True when the capture shows the app (not the black launch screen): the body of the screen is not black.
has_content() {
  python3 - "$1" <<'PY'
import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]).convert("RGB")
body = im.crop((0, int(im.height * 0.12), im.width, int(im.height * 0.9)))
mean = sum(ImageStat.Stat(body).mean) / 3
sys.exit(0 if mean > 8 else 1)
PY
}

# Launches the app and takes the screenshot once it has really drawn (slow CI simulators need > 4 s).
capture() {  # lang tab mode out
  local lang="$1" tab="$2" mode="$3" out="$4" attempt shot
  for attempt in 1 2 3; do
    xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
    # shellcheck disable=SC2086
    xcrun simctl launch "$UDID" "$BUNDLE" -LVLang "$lang" $mode -LVTab "$tab" >/dev/null
    for shot in $(seq 1 20); do
      sleep 2
      xcrun simctl io "$UDID" screenshot "$out" >/dev/null 2>&1 || continue
      if has_content "$out"; then
        sleep 2  # let the level meters / first animation frame settle
        xcrun simctl io "$UDID" screenshot "$out" >/dev/null
        echo "captured $lang tab $tab (attempt $attempt, ${shot}x2s)"
        return 0
      fi
    done
    echo "tab $tab ($lang): screen still black, relaunching" >&2
  done
  echo "FAILED: the app never drew tab $tab ($lang)" >&2
  xcrun simctl spawn "$UDID" log show --last 2m --style compact --predicate 'process == "LanVid Monitor"' 2>/dev/null | tail -40 >&2 || true
  return 1
}

for lang in en uk; do
  mkdir -p "$RAW/$lang"
  # tab order of the 4 screenshots: Connect, Mixer, Monitor FX, Settings
  for i in 0 1 2 3; do
    if [ "$i" = 0 ]; then
      MODE="-LVFakeStudios"           # Connect screen: discovered studios list
    else
      MODE="-LVDemo -LVMarketing"     # Mixer / Monitor FX / Settings: Demo Studio with the marketing preset
    fi
    capture "$lang" "$i" "$MODE" "$RAW/$lang/$((i + 1)).png"
  done
done
xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl status_bar "$UDID" clear 2>/dev/null || true

python3 "$HERE/scripts/compose_screenshots.py" "$RAW" "$HERE/fastlane/screenshots"
