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
for lang in en uk; do
  mkdir -p "$RAW/$lang"
  # tab order of the 4 screenshots: Connect, Mixer, Monitor FX, Settings
  for i in 0 1 2 3; do
    xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
    if [ "$i" = 0 ]; then
      MODE="-LVFakeStudios"           # Connect screen: discovered studios list
    else
      MODE="-LVDemo -LVMarketing"     # Mixer / Monitor FX / Settings: Demo Studio with the marketing preset
    fi
    # shellcheck disable=SC2086
    xcrun simctl launch "$UDID" "$BUNDLE" -LVLang "$lang" $MODE -LVTab "$i" >/dev/null
    sleep 4
    xcrun simctl io "$UDID" screenshot "$RAW/$lang/$((i + 1)).png" >/dev/null
  done
done
xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl status_bar "$UDID" clear 2>/dev/null || true

python3 "$HERE/scripts/compose_screenshots.py" "$RAW" "$HERE/fastlane/screenshots"
