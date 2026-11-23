#!/usr/bin/env bash
# LanVidSDK One-Click macOS Launcher (.command)
# Double-click in Finder on any MacBook (Apple Silicon M1-M4 or Intel):
#  - If LanVidSDK-v2.5.0-macOS.pkg is next to this script, opens it in macOS Installer.app.
#  - Otherwise, builds the Universal .pkg directly from this SDK tree and launches it!

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG_NEARBY="$HERE/LanVidSDK-v2.5.0-macOS.pkg"

echo "=================================================="
echo "  LanVidSDK v2.5.0 — macOS One-Click Installer"
echo "=================================================="

if [ -f "$PKG_NEARBY" ]; then
  echo "[LanVidSDK] Opening $PKG_NEARBY in Installer.app..."
  open "$PKG_NEARBY"
  exit 0
fi

# Locate build_macos_pkg.sh relative to sdk/apple/installer/
BUILDER=""
if [ -x "$HERE/../build_macos_pkg.sh" ]; then
  BUILDER="$HERE/../build_macos_pkg.sh"
elif [ -x "$HERE/build_macos_pkg.sh" ]; then
  BUILDER="$HERE/build_macos_pkg.sh"
fi

if [ -z "$BUILDER" ]; then
  echo "[ERROR] Neither LanVidSDK-v2.5.0-macOS.pkg nor build_macos_pkg.sh was found!"
  read -r -p "Press Enter to close..."
  exit 1
fi

echo "[LanVidSDK] Building universal macOS .pkg (arm64 NEON + x86_64 AVX2)..."
bash "$BUILDER"

ROOT_DIST="$(cd "$(dirname "$BUILDER")/../.." && pwd)/dist/apple/LanVidSDK-v2.5.0-macOS.pkg"
if [ -f "$ROOT_DIST" ]; then
  echo "[LanVidSDK] Launching $ROOT_DIST in Installer.app..."
  open "$ROOT_DIST"
fi
