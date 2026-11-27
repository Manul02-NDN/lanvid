#!/usr/bin/env bash
# Interactive & CLI Selectable SDK Installer (macOS / Linux)
# Lets the user install:
#   1) All together       — LanVid Video SDK (v3.0.0) + CrossLink Audio SDK (v1.0.0)
#   2) Video protocol     — LanVid Video + 2ch Stereo Audio SDK only
#   3) Audio protocol     — CrossLink 64ch AoIP + MIDI 2.0 + Monitor + Grid SDK only
#
# Usage:
#   ./scripts/install_sdk.sh                          # Interactive menu
#   ./scripts/install_sdk.sh --mode all|video|audio [--prefix /usr/local]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=""
PREFIX="/usr/local"

while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="${2:-}"; shift 2 ;;
    --mode=*) MODE="${1#*=}"; shift ;;
    --prefix) PREFIX="${2:-/usr/local}"; shift 2 ;;
    --prefix=*) PREFIX="${1#*=}"; shift ;;
    -h|--help)
      echo "Usage: $0 [--mode all|video|audio] [--prefix /usr/local]"
      exit 0 ;;
    *) echo "[ERROR] Unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$MODE" ]; then
  echo "================================================================"
  echo "  LanVid Video & CrossLink Audio SDK — Вибір протоколів"
  echo "================================================================"
  echo "  1) Встановити ВСЕ разом (LanVid Video v3.0.0 + CrossLink Audio v1.0.0)"
  echo "  2) Тільки відеопротокол LanVid (Video + 2ch Stereo Audio)"
  echo "  3) Тільки аудіопротокол CrossLink (64ch AoIP + MIDI 2.0 + Grid)"
  echo "----------------------------------------------------------------"
  read -r -p "Оберіть варіант [1-3] (за замовчуванням 1): " choice
  case "${choice:-1}" in
    1) MODE="all" ;;
    2) MODE="video" ;;
    3) MODE="audio" ;;
    *) echo "[ERROR] Невірний вибір: $choice" >&2; exit 2 ;;
  esac
fi

MODE_UPPER="$(echo "$MODE" | tr '[:lower:]' '[:upper:]')"
case "$MODE_UPPER" in
  ALL|VIDEO|AUDIO) ;;
  *) echo "[ERROR] Invalid --mode '$MODE'. Expected: all, video, audio." >&2; exit 2 ;;
esac

BUILD_DIR="$ROOT/build-install-$(echo "$MODE_UPPER" | tr '[:upper:]' '[:lower:]')"
GEN="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then GEN="Ninja"; fi

echo "[INFO] Configuring SDK (LANVID_SDK_COMPONENTS=$MODE_UPPER, prefix=$PREFIX)..."
cmake -S "$ROOT" -B "$BUILD_DIR" -G "$GEN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLANVID_SDK_COMPONENTS="$MODE_UPPER" \
  -DLANVID_SDK_CORE_ONLY=ON \
  -DLANVID_BUILD_SHARED=ON \
  -DLANVID_BUILD_TESTS=OFF

echo "[INFO] Building SDK..."
cmake --build "$BUILD_DIR" --parallel

echo "[INFO] Installing into $PREFIX..."
if [ -w "$PREFIX" ] || mkdir -p "$PREFIX" 2>/dev/null; then
  cmake --install "$BUILD_DIR" --prefix "$PREFIX"
else
  sudo cmake --install "$BUILD_DIR" --prefix "$PREFIX"
fi

rm -rf "$BUILD_DIR"
echo "[OK] Installation complete! Mode=$MODE_UPPER, Prefix=$PREFIX"
