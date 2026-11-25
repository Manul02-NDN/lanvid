#!/usr/bin/env bash
# LanVid Studio + LanVid Grid (L2 · 256ch) + DSP Server — Universal One-Click Linux Installer
# Supports Ubuntu/Debian/Mint (apt), Arch/Manjaro (pacman), Fedora (dnf), openSUSE (zypper).
# Auto-detects distro, installs SDL2/OpenGL/PipeWire/libcap dependencies, installs binaries & liblanvid_core.so,
# applies rootless Layer 2 capabilities (`setcap cap_net_raw,cap_net_admin=eip`), installs & enables
# systemd `lanvid-dsp.service`, deploys `obs-lanvid.so`, and configures UFW/firewalld.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="LanVid Studio"
PREFIX="${LANVID_PREFIX:-/usr/local}"
NON_INTERACTIVE=0
NO_LAUNCH=0

for arg in "$@"; do
  case "$arg" in
    --yes|-y|--non-interactive) NON_INTERACTIVE=1 ;;
    --no-launch) NO_LAUNCH=1 ;;
  esac
done

log()  { printf "\033[1;36m[LanVid]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[УВАГА]\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32m[OK]\033[0m %s\n" "$*"; }

# 1. Detect distribution & package manager
PKG_MGR=""
DISTRO="Linux"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  DISTRO="${PRETTY_NAME:-$NAME}"
fi
if command -v apt-get >/dev/null 2>&1; then PKG_MGR="apt"
elif command -v pacman >/dev/null 2>&1; then PKG_MGR="pacman"
elif command -v dnf >/dev/null 2>&1; then PKG_MGR="dnf"
elif command -v zypper >/dev/null 2>&1; then PKG_MGR="zypper"
fi
log "Виявлено ОС: $DISTRO (пакетний менеджер: ${PKG_MGR:-невідомий})"

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi

# 2. Check & close conflicting running LanVid processes
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  $SUDO systemctl stop lanvid-dsp.service 2>/dev/null || true
fi
for proc in lanvid_studio lanvid_app lanvid_dsp_server sender receiver lanvid-audio-bridge; do
  if pgrep -x "$proc" >/dev/null 2>&1; then
    warn "Знайдено запущений процес '$proc' — зупиняємо перед оновленням..."
    pkill -TERM -x "$proc" || true
    sleep 0.5
    pkill -KILL -x "$proc" 2>/dev/null || true
  fi
done
ok "Перевірка конфліктів процесів пройдена."

# 3. Check & auto-install runtime + build dependencies (SDL2, OpenGL, PipeWire/Pulse, X11, libcap2-bin)
NEED_BUILD=0
if [ ! -x "$SCRIPT_DIR/bin/lanvid_app" ]; then
  NEED_BUILD=1
fi

install_deps() {
  case "$PKG_MGR" in
    apt)
      local pkgs=(libsdl2-2.0-0 libgl1 libx11-6 libxrandr2 libcap2-bin)
      if command -v pipewire >/dev/null 2>&1; then pkgs+=(libpipewire-0.3-0); else pkgs+=(libpulse0); fi
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(build-essential cmake ninja-build pkg-config libsdl2-dev libgl1-mesa-dev libx11-dev libxrandr-dev)
      fi
      log "Перевірка/доустановка залежностей через apt: ${pkgs[*]}"
      $SUDO apt-get update -qq
      $SUDO apt-get install -y "${pkgs[@]}"
      ;;
    pacman)
      local pkgs=(sdl2 libglvnd libx11 libxrandr pipewire libcap)
      if [ "$NEED_BUILD" -eq 1 ]; then pkgs+=(base-devel cmake ninja pkgconf); fi
      log "Перевірка/доустановка залежностей через pacman: ${pkgs[*]}"
      $SUDO pacman -Sy --needed --noconfirm "${pkgs[@]}"
      ;;
    dnf)
      local pkgs=(SDL2 mesa-libGL libX11 libXrandr pipewire-libs libcap)
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(gcc-c++ cmake ninja-build pkgconf-pkg-config SDL2-devel mesa-libGL-devel libX11-devel libXrandr-devel)
      fi
      log "Перевірка/доустановка залежностей через dnf: ${pkgs[*]}"
      $SUDO dnf install -y "${pkgs[@]}"
      ;;
    zypper)
      local pkgs=(libSDL2-2_0-0 Mesa-libGL1 libX11-6 libXrandr2 pipewire libcap-progs)
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(gcc-c++ cmake ninja pkg-config libSDL2-devel Mesa-libGL-devel libX11-devel libXrandr-devel)
      fi
      log "Перевірка/доустановка залежностей через zypper: ${pkgs[*]}"
      $SUDO zypper --non-interactive install "${pkgs[@]}"
      ;;
    *)
      warn "Невідомий пакетний менеджер; переконайтесь, що встановлено SDL2, OpenGL, PipeWire/PulseAudio, X11 та libcap."
      ;;
  esac
}

install_deps
ok "Системні залежності перевірено та встановлено."

# 4. Build natively if prebuilt binaries are not bundled in payload
BIN_SRC="$SCRIPT_DIR/bin"
LIB_SRC="$SCRIPT_DIR/lib"
if [ "$NEED_BUILD" -eq 1 ]; then
  if [ ! -f "$SCRIPT_DIR/src_bundle/CMakeLists.txt" ]; then
    echo "ERROR: neither prebuilt bin/lanvid_app nor src_bundle/CMakeLists.txt found!" >&2
    exit 1
  fi
  log "Компіляція LanVid Studio + Grid DSP Server під поточну архітектуру ($(uname -m))..."
  BUILD_TMP="$SCRIPT_DIR/_build"
  cmake -S "$SCRIPT_DIR/src_bundle" -B "$BUILD_TMP" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DLANVID_BUILD_TESTS=OFF -DLANVID_BUILD_GUI=ON -DLANVID_BUILD_CLI=ON -DLANVID_BUILD_SHARED=ON
  cmake --build "$BUILD_TMP" --parallel "$(nproc)"
  mkdir -p "$BIN_SRC" "$LIB_SRC"
  for b in lanvid_app lanvid_dsp_server sender receiver lanvid-audio-bridge; do
    [ -f "$BUILD_TMP/$b" ] && cp "$BUILD_TMP/$b" "$BIN_SRC/$b"
  done
  [ -f "$BUILD_TMP/lanvid_app" ] && cp "$BUILD_TMP/lanvid_app" "$BIN_SRC/lanvid_studio"
  [ -f "$BUILD_TMP/liblanvid_core.so" ] && cp "$BUILD_TMP/liblanvid_core.so" "$LIB_SRC/liblanvid_core.so"
  ok "Нативна збірка завершена."
fi

# 5. Check UDP port 5000 conflict & persist default config
CHOSEN_PORT=5000
port_busy() {
  local p="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -uln "( sport = :$p )" 2>/dev/null | grep -q ":$p"
  else
    return 1
  fi
}
if port_busy 5000; then
  warn "UDP порт 5000 вже зайнятий іншою програмою — шукаємо вільний порт..."
  for cand in $(seq 5002 2 5100); do
    if ! port_busy "$cand"; then
      CHOSEN_PORT="$cand"
      break
    fi
  done
  warn "Призначено вільний порт за замовчуванням: $CHOSEN_PORT"
else
  ok "UDP порт 5000 вільний."
fi

CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/lanvid"
CFG_FILE="$CFG_DIR/config.json"
mkdir -p "$CFG_DIR"
if [ ! -f "$CFG_FILE" ]; then
  cat > "$CFG_FILE" <<EOF
{
  "dest_ip": "127.0.0.1",
  "dest_port": $CHOSEN_PORT,
  "listen_port": $CHOSEN_PORT,
  "preview_backend": "auto",
  "quality": 80,
  "fps": 60,
  "volume": 1.0
}
EOF
fi

# 6. Install binaries, shared library, icon, and .desktop launcher
log "Встановлення файлів у $PREFIX..."
$SUDO install -d "$PREFIX/bin" "$PREFIX/lib" /usr/share/applications /usr/share/icons/hicolor/scalable/apps
for b in lanvid_studio lanvid_app lanvid_dsp_server sender receiver lanvid-audio-bridge; do
  if [ -f "$BIN_SRC/$b" ]; then
    $SUDO install -m 0755 "$BIN_SRC/$b" "$PREFIX/bin/$b"
  fi
done
if [ ! -f "$PREFIX/bin/lanvid_studio" ] && [ -f "$PREFIX/bin/lanvid_app" ]; then
  $SUDO install -m 0755 "$PREFIX/bin/lanvid_app" "$PREFIX/bin/lanvid_studio"
fi
if [ -f "$LIB_SRC/liblanvid_core.so" ]; then
  $SUDO install -m 0755 "$LIB_SRC/liblanvid_core.so" "$PREFIX/lib/liblanvid_core.so"
  $SUDO ldconfig 2>/dev/null || true
fi
if [ -f "$SCRIPT_DIR/lanvid.desktop" ]; then
  $SUDO install -m 0644 "$SCRIPT_DIR/lanvid.desktop" /usr/share/applications/lanvid.desktop
fi
if [ -f "$SCRIPT_DIR/lanvid.svg" ]; then
  $SUDO install -m 0644 "$SCRIPT_DIR/lanvid.svg" /usr/share/icons/hicolor/scalable/apps/lanvid.svg
fi
ok "Ярлик «$APP_NAME» додано до меню програм."

# 7. Grant Layer 2 Raw Sockets (AF_PACKET) & QoS SO_PRIORITY=6 capabilities without root
if command -v setcap >/dev/null 2>&1; then
  log "Надання прав Layer 2 Raw Sockets (cap_net_raw,cap_net_admin=eip)..."
  [ -x "$PREFIX/bin/lanvid_studio" ]     && $SUDO setcap cap_net_raw,cap_net_admin=eip "$PREFIX/bin/lanvid_studio" || true
  [ -x "$PREFIX/bin/lanvid_app" ]        && $SUDO setcap cap_net_raw,cap_net_admin=eip "$PREFIX/bin/lanvid_app" || true
  [ -x "$PREFIX/bin/lanvid_dsp_server" ] && $SUDO setcap cap_net_raw,cap_net_admin=eip "$PREFIX/bin/lanvid_dsp_server" || true
  ok "Права Layer 2 (AF_PACKET + QoS) налаштовано без потреби запускати від root."
else
  warn "Утиліта setcap не знайдена; встановіть libcap2-bin для роботи LanVid Grid L2 без root."
fi

# 8. Install & enable systemd service for LanVid Grid DSP Server
if [ -x "$PREFIX/bin/lanvid_dsp_server" ] && [ -d /etc/systemd/system ]; then
  log "Встановлення systemd-служби /etc/systemd/system/lanvid-dsp.service..."
  if [ -f "$SCRIPT_DIR/systemd/lanvid-dsp.service" ]; then
    $SUDO install -m 0644 "$SCRIPT_DIR/systemd/lanvid-dsp.service" /etc/systemd/system/lanvid-dsp.service
  else
    $SUDO tee /etc/systemd/system/lanvid-dsp.service >/dev/null <<EOF
[Unit]
Description=LanVid Grid 256-Channel Layer 2 Distributed DSP Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$PREFIX/bin/lanvid_dsp_server --name "LanVid Grid DSP"
Restart=always
RestartSec=3
AmbientCapabilities=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE
CapabilityBoundingSet=CAP_NET_RAW CAP_NET_ADMIN CAP_SYS_NICE
LimitRTPRIO=95
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
EOF
  fi
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    $SUDO systemctl daemon-reload || true
    $SUDO systemctl enable --now lanvid-dsp.service || true
    ok "Службу lanvid-dsp.service активовано та запущено."
  fi
fi

# 9. Auto-install OBS Studio plugin (system-wide and/or ~/.config/obs-studio)
if [ -f "$SCRIPT_DIR/obs-lanvid/obs-lanvid.so" ]; then
  if [ -d /usr/lib/x86_64-linux-gnu/obs-plugins ] || [ -d /usr/share/obs ]; then
    $SUDO install -d /usr/lib/x86_64-linux-gnu/obs-plugins /usr/share/obs/obs-plugins/obs-lanvid/locale
    $SUDO install -m 0755 "$SCRIPT_DIR/obs-lanvid/obs-lanvid.so" /usr/lib/x86_64-linux-gnu/obs-plugins/obs-lanvid.so
    if [ -d "$SCRIPT_DIR/obs-lanvid/locale" ]; then
      $SUDO install -m 0644 "$SCRIPT_DIR/obs-lanvid/locale/"*.ini /usr/share/obs/obs-plugins/obs-lanvid/locale/ 2>/dev/null || true
    fi
    ok "Плагін obs-lanvid.so встановлено системно у /usr/lib/x86_64-linux-gnu/obs-plugins/."
  fi
  OBS_USER_CFG="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio"
  if [ -d "$OBS_USER_CFG" ]; then
    mkdir -p "$OBS_USER_CFG/plugins/obs-lanvid/bin/64bit" "$OBS_USER_CFG/plugins/obs-lanvid/data/locale"
    cp -f "$SCRIPT_DIR/obs-lanvid/obs-lanvid.so" "$OBS_USER_CFG/plugins/obs-lanvid/bin/64bit/obs-lanvid.so"
    if [ -d "$SCRIPT_DIR/obs-lanvid/locale" ]; then
      cp -f "$SCRIPT_DIR/obs-lanvid/locale/"*.ini "$OBS_USER_CFG/plugins/obs-lanvid/data/locale/" 2>/dev/null || true
    fi
    ok "Плагін obs-lanvid.so встановлено у профіль користувача OBS ($OBS_USER_CFG)."
  fi
fi

# 10. Configure Linux Firewall (UFW or firewalld) for UDP streams + LAN discovery + mDNS
if command -v ufw >/dev/null 2>&1 && $SUDO ufw status 2>/dev/null | grep -qi "Status: active"; then
  log "Додавання UDP-правил до UFW (порти 5000:5035, 5099, 5353 mDNS)..."
  $SUDO ufw allow 5000:5035/udp comment "LanVid Studio Audio/Video/Monitor" >/dev/null || true
  $SUDO ufw allow 5099/udp comment "LanVid LAN Discovery" >/dev/null || true
  $SUDO ufw allow 5353/udp comment "LanVid mDNS" >/dev/null || true
  ok "Правила UFW налаштовано."
elif command -v firewall-cmd >/dev/null 2>&1 && $SUDO firewall-cmd --state >/dev/null 2>&1; then
  log "Додавання UDP-правил до firewalld..."
  $SUDO firewall-cmd --permanent --add-port="5000-5035/udp" >/dev/null || true
  $SUDO firewall-cmd --permanent --add-port="5099/udp" >/dev/null || true
  $SUDO firewall-cmd --permanent --add-port="5353/udp" >/dev/null || true
  $SUDO firewall-cmd --reload >/dev/null || true
  ok "Правила firewalld налаштовано."
fi

ok "=== Встановлення $APP_NAME + Grid L2 завершено успішно! ==="
if [ "$NO_LAUNCH" -eq 0 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x "$PREFIX/bin/lanvid_studio" ]; then
  log "Запуск LanVid Studio..."
  nohup "$PREFIX/bin/lanvid_studio" >/dev/null 2>&1 &
fi
