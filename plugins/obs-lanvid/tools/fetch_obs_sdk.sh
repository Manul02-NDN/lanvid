#!/usr/bin/env bash
# Prepares a light OBS SDK for building obs-lanvid against the OBS Studio installed on this Mac:
#   headers of the matching obs-studio tag (libobs + obs-frontend-api.h + obsconfig.h) + simde (ARM).
# Usage:  plugins/obs-lanvid/tools/fetch_obs_sdk.sh [out_dir] [OBS.app]
#         cmake ... -DBUILD_OBS_PLUGIN=ON -DLANVID_OBS_SDK_DIR=<out_dir>
set -euo pipefail
OUT="${1:-$(cd "$(dirname "$0")/../../.." && pwd)/build-obs-sdk}"
APP="${2:-/Applications/OBS.app}"
VER="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "OBS Studio $VER at $APP -> $OUT"
git clone -q --depth 1 --branch "$VER" --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git "$TMP/obs"
git -C "$TMP/obs" sparse-checkout set libobs frontend/api UI/obs-frontend-api
git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git "$TMP/simde"
rm -rf "$OUT" && mkdir -p "$OUT/include"
rsync -a --include='*/' --include='*.h' --include='*.hpp' --exclude='*' "$TMP/obs/libobs/" "$OUT/include/"
FE="$(find "$TMP/obs" -name obs-frontend-api.h | head -n 1)"
cp "$FE" "$OUT/include/"
cp -R "$TMP/simde/simde" "$OUT/include/"
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > "$OUT/include/obsconfig.h"
echo "$VER" > "$OUT/OBS_VERSION"
echo "Light OBS SDK ready: -DLANVID_OBS_SDK_DIR=$OUT"
