#!/usr/bin/env bash
# Builds native Linux packages (.deb, .AppImage, .tar.gz, and self-extracting LanVid-Linux-x86_64.run)
# Full ecosystem install out-of-the-box:
#   - lanvid_studio & lanvid_app, lanvid_dsp_server, sender, receiver, lanvid-audio-bridge -> /usr/local/bin/
#   - liblanvid_core.so -> /usr/local/lib/ + ldconfig
#   - Rootless Layer 2 AF_PACKET + QoS SO_PRIORITY=6 via setcap cap_net_raw,cap_net_admin=eip
#   - Systemd unit /etc/systemd/system/lanvid-dsp.service (systemctl daemon-reload && systemctl enable --now)
#   - Auto-deployed OBS Studio plugin (/usr/lib/x86_64-linux-gnu/obs-plugins/ & ~/.config/obs-studio/plugins/obs-lanvid/bin/64bit/)
#   - UFW / firewalld rules for UDP 5000-5035, 5099 & 5353 (mDNS)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/installer/linux"
BUILD="$ROOT/build-linux"
VER="$(sed -n 's/^project(lanvid VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
[ -n "$VER" ] || { echo "Cannot read the project version from CMakeLists.txt" >&2; exit 1; }
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLANVID_BUILD_GUI=ON -DLANVID_BUILD_CLI=ON -DLANVID_BUILD_SHARED=ON \
      -DLANVID_BUILD_TESTS=ON -DBUILD_OBS_PLUGIN=ON
cmake --build "$BUILD" --parallel "$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure

# Generate canonical systemd unit for LanVid Grid DSP Server
DSP_UNIT_TMP="$BUILD/lanvid-dsp.service"
cat > "$DSP_UNIT_TMP" <<'UNITEOF'
[Unit]
Description=LanVid Grid 256-Channel Layer 2 Distributed DSP Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/lanvid_dsp_server --name "LanVid Grid DSP"
Restart=always
RestartSec=3
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE
LimitRTPRIO=95
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
UNITEOF

# 1. Build .deb package
DEB_ROOT="$BUILD/deb_pkg"
rm -rf "$DEB_ROOT"
install -d "$DEB_ROOT/DEBIAN" \
           "$DEB_ROOT/usr/local/bin" \
           "$DEB_ROOT/usr/local/lib" \
           "$DEB_ROOT/etc/systemd/system" \
           "$DEB_ROOT/usr/share/applications" \
           "$DEB_ROOT/usr/share/icons/hicolor/scalable/apps" \
           "$DEB_ROOT/usr/share/obs/obs-plugins/obs-lanvid/locale"

install -m 0755 "$BUILD/lanvid_app"          "$DEB_ROOT/usr/local/bin/lanvid_app"
install -m 0755 "$BUILD/lanvid_app"          "$DEB_ROOT/usr/local/bin/lanvid_studio"
[ -f "$BUILD/lanvid_audio_studio" ] && install -m 0755 "$BUILD/lanvid_audio_studio" "$DEB_ROOT/usr/local/bin/lanvid_audio_studio"
[ -f "$BUILD/lanvid_video_studio" ] && install -m 0755 "$BUILD/lanvid_video_studio" "$DEB_ROOT/usr/local/bin/lanvid_video_studio"
install -m 0755 "$BUILD/lanvid_dsp_server"   "$DEB_ROOT/usr/local/bin/lanvid_dsp_server"
install -m 0755 "$BUILD/sender"              "$DEB_ROOT/usr/local/bin/sender"
install -m 0755 "$BUILD/receiver"            "$DEB_ROOT/usr/local/bin/receiver"
install -m 0755 "$BUILD/lanvid-audio-bridge" "$DEB_ROOT/usr/local/bin/lanvid-audio-bridge"
install -m 0755 "$BUILD/liblanvid_core.so"   "$DEB_ROOT/usr/local/lib/liblanvid_core.so"
install -m 0644 "$DSP_UNIT_TMP"              "$DEB_ROOT/etc/systemd/system/lanvid-dsp.service"
install -m 0644 "$OUT/lanvid.desktop"        "$DEB_ROOT/usr/share/applications/lanvid.desktop"
[ -f "$OUT/lanvid-audio.desktop" ] && install -m 0644 "$OUT/lanvid-audio.desktop" "$DEB_ROOT/usr/share/applications/lanvid-audio.desktop"
[ -f "$OUT/lanvid-video.desktop" ] && install -m 0644 "$OUT/lanvid-video.desktop" "$DEB_ROOT/usr/share/applications/lanvid-video.desktop"
install -m 0644 "$OUT/lanvid.svg"            "$DEB_ROOT/usr/share/icons/hicolor/scalable/apps/lanvid.svg"

if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  install -m 0644 "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$DEB_ROOT/usr/share/obs/obs-plugins/obs-lanvid/locale/"
fi
if [ -f "$BUILD/plugins/obs-lanvid/obs-lanvid.so" ]; then
  install -d "$DEB_ROOT/usr/lib/x86_64-linux-gnu/obs-plugins"
  install -m 0755 "$BUILD/plugins/obs-lanvid/obs-lanvid.so" "$DEB_ROOT/usr/lib/x86_64-linux-gnu/obs-plugins/obs-lanvid.so"
fi

cat > "$DEB_ROOT/DEBIAN/control" <<EOF
Package: lanvid-studio
Version: $VER
Section: video
Priority: optional
Architecture: $ARCH
Depends: libsdl2-2.0-0, libgl1, libx11-6, libxrandr2, libstdc++6
Recommends: libcap2-bin
Maintainer: LanVid Project <dev@lanvid.local>
Description: Ultra-Low Latency LAN Video, 64ch Classic AoIP & 256ch Grid Layer 2 Audio Studio
 Intra-only AVX2/NEON slice codec, LanVid Grid Layer 2 (256ch + DSP Server),
 UDP + XOR FEC + AIMD, Dear ImGui Studio GUI, OBS Studio plugin & JACK/PipeWire bridge.
EOF

cat > "$DEB_ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

# 1. Refresh dynamic linker cache for /usr/local/lib/liblanvid_core.so
ldconfig || true

# 2. Grant Layer 2 Raw Socket (AF_PACKET) & QoS SO_PRIORITY=6 capabilities without root
if command -v setcap >/dev/null 2>&1; then
  setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_studio || true
  setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_app || true
  [ -x /usr/local/bin/lanvid_audio_studio ] && setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_audio_studio || true
  [ -x /usr/local/bin/lanvid_video_studio ] && setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_video_studio || true
  setcap cap_net_raw,cap_net_admin=eip /usr/local/bin/lanvid_dsp_server || true
fi

# 3. Enable & start systemd service for LanVid Grid DSP Server
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  systemctl daemon-reload || true
  systemctl enable --now lanvid-dsp.service || true
fi

# 4. Auto-copy OBS Studio plugin to user profile if ~/.config/obs-studio exists
OBS_SO="/usr/lib/x86_64-linux-gnu/obs-plugins/obs-lanvid.so"
TARGET_USER="${SUDO_USER:-}"
if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != "root" ]; then
  USER_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6 || echo "/home/$TARGET_USER")"
  if [ -d "$USER_HOME/.config/obs-studio" ] && [ -f "$OBS_SO" ]; then
    OBS_USER_BIN="$USER_HOME/.config/obs-studio/plugins/obs-lanvid/bin/64bit"
    OBS_USER_LOC="$USER_HOME/.config/obs-studio/plugins/obs-lanvid/data/locale"
    mkdir -p "$OBS_USER_BIN" "$OBS_USER_LOC"
    cp -f "$OBS_SO" "$OBS_USER_BIN/obs-lanvid.so" || true
    cp -f /usr/share/obs/obs-plugins/obs-lanvid/locale/*.ini "$OBS_USER_LOC/" 2>/dev/null || true
    chown -R "$TARGET_USER:$TARGET_USER" "$USER_HOME/.config/obs-studio/plugins/obs-lanvid" 2>/dev/null || true
  fi
fi

# 5. Open UFW / firewalld ports for LanVid Classic UDP (5000-5035, 5099, 5353 mDNS)
if command -v ufw >/dev/null 2>&1 && ufw status | grep -qi "Status: active"; then
  ufw allow 5000:5035/udp >/dev/null 2>&1 || true
  ufw allow 5099/udp >/dev/null 2>&1 || true
  ufw allow 5353/udp >/dev/null 2>&1 || true
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  firewall-cmd --permanent --add-port=5000-5035/udp >/dev/null 2>&1 || true
  firewall-cmd --permanent --add-port=5099/udp >/dev/null 2>&1 || true
  firewall-cmd --permanent --add-port=5353/udp >/dev/null 2>&1 || true
  firewall-cmd --reload >/dev/null 2>&1 || true
fi
EOF
chmod 0755 "$DEB_ROOT/DEBIAN/postinst"

cat > "$DEB_ROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  systemctl stop lanvid-dsp.service || true
  systemctl disable lanvid-dsp.service || true
fi
EOF
chmod 0755 "$DEB_ROOT/DEBIAN/prerm"

if command -v dpkg-deb >/dev/null 2>&1; then
  dpkg-deb --build "$DEB_ROOT" "$OUT/lanvid-studio_${VER}_${ARCH}.deb"
  echo "Built $OUT/lanvid-studio_${VER}_${ARCH}.deb"
fi

# 2. Build AppImage (if appimagetool is available)
APPDIR="$BUILD/LanVid.AppDir"
rm -rf "$APPDIR"
install -d "$APPDIR/usr/bin" "$APPDIR/usr/lib"
install -m 0755 "$BUILD/lanvid_app"        "$APPDIR/usr/bin/lanvid_app"
install -m 0755 "$BUILD/lanvid_dsp_server" "$APPDIR/usr/bin/lanvid_dsp_server"
install -m 0755 "$BUILD/liblanvid_core.so" "$APPDIR/usr/lib/liblanvid_core.so"
install -m 0644 "$OUT/lanvid.desktop"      "$APPDIR/lanvid.desktop"
install -m 0644 "$OUT/lanvid.svg"          "$APPDIR/lanvid.svg"
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

# 3. Build self-extracting binary LanVid-Linux-x86_64.run & versioned .tar.gz
STAGE="$BUILD/run_stage"
rm -rf "$STAGE"
install -d "$STAGE/bin" "$STAGE/lib" "$STAGE/systemd" "$STAGE/obs-lanvid/locale"
cp "$BUILD/lanvid_app" "$BUILD/lanvid_dsp_server" "$BUILD/sender" "$BUILD/receiver" "$BUILD/lanvid-audio-bridge" "$STAGE/bin/"
cp "$BUILD/lanvid_app" "$STAGE/bin/lanvid_studio"
cp "$BUILD/liblanvid_core.so" "$STAGE/lib/"
cp "$DSP_UNIT_TMP" "$STAGE/systemd/lanvid-dsp.service"
if [ -d "$ROOT/plugins/obs-lanvid/data/locale" ]; then
  cp "$ROOT/plugins/obs-lanvid/data/locale/"*.ini "$STAGE/obs-lanvid/locale/"
fi
if [ -f "$BUILD/plugins/obs-lanvid/obs-lanvid.so" ]; then
  cp "$BUILD/plugins/obs-lanvid/obs-lanvid.so" "$STAGE/obs-lanvid/obs-lanvid.so"
fi
cp "$OUT/install_lanvid.sh" "$OUT/lanvid.desktop" "$OUT/lanvid.svg" "$STAGE/"
chmod +x "$STAGE/install_lanvid.sh"

PAYLOAD="$BUILD/payload.tar.gz"
tar -czf "$PAYLOAD" -C "$STAGE" .
cp "$PAYLOAD" "$OUT/lanvid-v${VER}-linux-x86_64.tar.gz"
echo "Built $OUT/lanvid-v${VER}-linux-x86_64.tar.gz"
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
