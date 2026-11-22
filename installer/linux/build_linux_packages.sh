#!/usr/bin/env bash
# Builds native Linux packages (.deb, .AppImage, and prebuilt LanVid-Linux-x86_64.run) on a Linux host/CI.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/installer/linux"
BUILD="$ROOT/build-linux"
VER="2.3.1"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLANVID_BUILD_GUI=ON -DLANVID_BUILD_SHARED=ON -DLANVID_BUILD_TESTS=ON
cmake --build "$BUILD" --parallel "$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure

# 1. Build .deb package
DEB_ROOT="$BUILD/deb_pkg"
rm -rf "$DEB_ROOT"
install -d "$DEB_ROOT/DEBIAN" "$DEB_ROOT/usr/local/bin" "$DEB_ROOT/usr/local/lib" \
           "$DEB_ROOT/usr/share/applications" "$DEB_ROOT/usr/share/icons/hicolor/scalable/apps"
install -m 0755 "$BUILD/lanvid_app" "$DEB_ROOT/usr/local/bin/lanvid_app"
install -m 0755 "$BUILD/sender"     "$DEB_ROOT/usr/local/bin/sender"
install -m 0755 "$BUILD/receiver"   "$DEB_ROOT/usr/local/bin/receiver"
install -m 0755 "$BUILD/lanvid-audio-bridge" "$DEB_ROOT/usr/local/bin/lanvid-audio-bridge"
install -m 0755 "$BUILD/liblanvid_core.so" "$DEB_ROOT/usr/local/lib/liblanvid_core.so"
install -m 0644 "$OUT/lanvid.desktop" "$DEB_ROOT/usr/share/applications/lanvid.desktop"
install -m 0644 "$OUT/lanvid.svg"     "$DEB_ROOT/usr/share/icons/hicolor/scalable/apps/lanvid.svg"

cat > "$DEB_ROOT/DEBIAN/control" <<EOF
Package: lanvid-studio
Version: $VER
Section: video
Priority: optional
Architecture: $ARCH
Depends: libsdl2-2.0-0, libgl1, libx11-6, libxrandr2, libstdc++6
Maintainer: LanVid Project <dev@lanvid.local>
Description: Ultra-Low Latency LAN Video & Audio Streaming Studio
 Intra-only AVX2/NEON slice codec, UDP + XOR FEC + AIMD, Dear ImGui Studio GUI.
EOF
cat > "$DEB_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig || true
if command -v ufw >/dev/null 2>&1 && ufw status | grep -qi "Status: active"; then
  ufw allow 5000/udp >/dev/null 2>&1 || true
  ufw allow 5099/udp >/dev/null 2>&1 || true
fi
EOF
chmod 0755 "$DEB_ROOT/DEBIAN/postinst"
if command -v dpkg-deb >/dev/null 2>&1; then
  dpkg-deb --build "$DEB_ROOT" "$OUT/lanvid-studio_${VER}_${ARCH}.deb"
  echo "Built $OUT/lanvid-studio_${VER}_${ARCH}.deb"
fi

# 2. Build AppImage (if appimagetool is available)
APPDIR="$BUILD/LanVid.AppDir"
rm -rf "$APPDIR"
install -d "$APPDIR/usr/bin" "$APPDIR/usr/lib"
install -m 0755 "$BUILD/lanvid_app" "$APPDIR/usr/bin/lanvid_app"
install -m 0755 "$BUILD/liblanvid_core.so" "$APPDIR/usr/lib/liblanvid_core.so"
install -m 0644 "$OUT/lanvid.desktop" "$APPDIR/lanvid.desktop"
install -m 0644 "$OUT/lanvid.svg"     "$APPDIR/lanvid.svg"
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/lanvid_app" "$@"
EOF
chmod 0755 "$APPDIR/AppRun"
if command -v appimagetool >/dev/null 2>&1; then
  appimagetool "$APPDIR" "$OUT/LanVid-Studio-${VER}-x86_64.AppImage"
fi

# 3. Build self-extracting binary LanVid-Linux-x86_64.run
STAGE="$BUILD/run_stage"
rm -rf "$STAGE"
install -d "$STAGE/bin" "$STAGE/lib"
cp "$BUILD/lanvid_app" "$BUILD/sender" "$BUILD/receiver" "$BUILD/lanvid-audio-bridge" "$STAGE/bin/"
cp "$BUILD/liblanvid_core.so" "$STAGE/lib/"
cp "$OUT/install_lanvid.sh" "$OUT/lanvid.desktop" "$OUT/lanvid.svg" "$STAGE/"
chmod +x "$STAGE/install_lanvid.sh"

PAYLOAD="$BUILD/payload.tar.gz"
tar -czf "$PAYLOAD" -C "$STAGE" .
cat > "$OUT/LanVid-Linux-x86_64.run" <<'HEADER'
#!/usr/bin/env bash
set -euo pipefail
TMPDIR="$(mktemp -d /tmp/lanvid-install.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT
ARCHIVE_LINE="$(awk '/^__LANVID_ARCHIVE_BELOW__$/ {print NR + 1; exit 0;}' "$0")"
tail -n +"$ARCHIVE_LINE" "$0" | tar -xz -C "$TMPDIR"
exec bash "$TMPDIR/install_lanvid.sh" "$@"
__LANVID_ARCHIVE_BELOW__
HEADER
cat "$PAYLOAD" >> "$OUT/LanVid-Linux-x86_64.run"
chmod +x "$OUT/LanVid-Linux-x86_64.run"
echo "Built $OUT/LanVid-Linux-x86_64.run"
