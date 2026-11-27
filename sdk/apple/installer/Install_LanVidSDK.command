#!/usr/bin/env bash
# LanVid Video & CrossLink Audio SDK — macOS One-Click Installer Launcher
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG_NAME="LanVidCrossLinkSDK-v3.0.0-macOS.pkg"
PKG_NEARBY="$HERE/$PKG_NAME"

echo "============================================================"
echo "  LanVid Video (v3.0.0) & CrossLink Audio (v1.0.0) Installer"
echo "============================================================"

if [ -f "$PKG_NEARBY" ]; then
  open "$PKG_NEARBY"
  exit 0
fi

BUILDER=""
if [ -x "$HERE/../build_macos_pkg.sh" ]; then
  BUILDER="$HERE/../build_macos_pkg.sh"
elif [ -x "$HERE/build_macos_pkg.sh" ]; then
  BUILDER="$HERE/build_macos_pkg.sh"
fi

if [ -z "$BUILDER" ]; then
  echo "[ERROR] Neither $PKG_NAME nor build_macos_pkg.sh was found!"
  read -r -p "Press Enter to close..."
  exit 1
fi

bash "$BUILDER"
ROOT_DIST="$(cd "$(dirname "$BUILDER")/../.." && pwd)/dist/apple/$PKG_NAME"
[ -f "$ROOT_DIST" ] && open "$ROOT_DIST"
