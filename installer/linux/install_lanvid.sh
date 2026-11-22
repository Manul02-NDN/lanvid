#!/usr/bin/env bash
# LanVid Studio — Universal One-Click Linux Installer
# Supports Ubuntu/Debian/Mint (apt), Arch/Manjaro (pacman), Fedora (dnf), openSUSE (zypper).
# Auto-detects distro, checks & installs missing SDL2/OpenGL/PipeWire/Pulse/X11 dependencies,
# resolves running process & UDP port 5000 conflicts, configures UFW/firewalld, installs binaries
# and desktop entry, and optionally launches LanVid Studio.

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
for proc in lanvid_app sender receiver lanvid-audio-bridge; do
  if pgrep -x "$proc" >/dev/null 2>&1; then
    warn "Знайдено запущений процес '$proc' — зупиняємо перед оновленням..."
    pkill -TERM -x "$proc" || true
    sleep 0.5
    pkill -KILL -x "$proc" 2>/dev/null || true
  fi
done
ok "Перевірка конфліктів процесів пройдена."

# 3. Check & auto-install runtime + build dependencies (SDL2, OpenGL, PipeWire/Pulse, X11)
NEED_BUILD=0
if [ ! -x "$SCRIPT_DIR/bin/lanvid_app" ]; then
  NEED_BUILD=1
fi

install_deps() {
  case "$PKG_MGR" in
    apt)
      local pkgs=(libsdl2-2.0-0 libgl1 libx11-6 libxrandr2)
      if command -v pipewire >/dev/null 2>&1; then pkgs+=(libpipewire-0.3-0); else pkgs+=(libpulse0); fi
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(build-essential cmake ninja-build pkg-config libsdl2-dev libgl1-mesa-dev libx11-dev libxrandr-dev)
      fi
      log "Перевірка/доустановка залежностей через apt: ${pkgs[*]}"
      $SUDO apt-get update -qq
      $SUDO apt-get install -y "${pkgs[@]}"
      ;;
    pacman)
      local pkgs=(sdl2 libglvnd libx11 libxrandr pipewire)
      if [ "$NEED_BUILD" -eq 1 ]; then pkgs+=(base-devel cmake ninja pkgconf); fi
      log "Перевірка/доустановка залежностей через pacman: ${pkgs[*]}"
      $SUDO pacman -Sy --needed --noconfirm "${pkgs[@]}"
      ;;
    dnf)
      local pkgs=(SDL2 mesa-libGL libX11 libXrandr pipewire-libs)
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(gcc-c++ cmake ninja-build pkgconf-pkg-config SDL2-devel mesa-libGL-devel libX11-devel libXrandr-devel)
      fi
      log "Перевірка/доустановка залежностей через dnf: ${pkgs[*]}"
      $SUDO dnf install -y "${pkgs[@]}"
      ;;
    zypper)
      local pkgs=(libSDL2-2_0-0 Mesa-libGL1 libX11-6 libXrandr2 pipewire)
      if [ "$NEED_BUILD" -eq 1 ]; then
        pkgs+=(gcc-c++ cmake ninja pkg-config libSDL2-devel Mesa-libGL-devel libX11-devel libXrandr-devel)
      fi
      log "Перевірка/доустановка залежностей через zypper: ${pkgs[*]}"
      $SUDO zypper --non-interactive install "${pkgs[@]}"
      ;;
    *)
      warn "Невідомий пакетний менеджер; переконайтесь, що встановлено SDL2, OpenGL, PipeWire/PulseAudio та X11."
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
  log "Компіляція LanVid Studio під поточну архітектуру ($(uname -m), AVX2/NEON auto-detect)..."
  BUILD_TMP="$SCRIPT_DIR/_build"
  cmake -S "$SCRIPT_DIR/src_bundle" -B "$BUILD_TMP" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DLANVID_BUILD_TESTS=OFF -DLANVID_BUILD_GUI=ON -DLANVID_BUILD_SHARED=ON
  cmake --build "$BUILD_TMP" --parallel "$(nproc)"
  mkdir -p "$BIN_SRC" "$LIB_SRC"
  for b in lanvid_app sender receiver lanvid-audio-bridge; do
    [ -f "$BUILD_TMP/$b" ] && cp "$BUILD_TMP/$b" "$BIN_SRC/$b"
  done
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
for b in lanvid_app sender receiver lanvid-audio-bridge; do
  if [ -f "$BIN_SRC/$b" ]; then
    $SUDO install -m 0755 "$BIN_SRC/$b" "$PREFIX/bin/$b"
  fi
done
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

# 7. Configure Linux Firewall (UFW or firewalld) for UDP stream + LAN discovery
if command -v ufw >/dev/null 2>&1 && $SUDO ufw status 2>/dev/null | grep -qi "Status: active"; then
  log "Додавання UDP-правил до UFW (порти $CHOSEN_PORT та 5099)..."
  $SUDO ufw allow "${CHOSEN_PORT}/udp" comment "LanVid Studio Video/Audio" >/dev/null || true
  $SUDO ufw allow 5099/udp comment "LanVid LAN Discovery" >/dev/null || true
  ok "Правила UFW налаштовано."
elif command -v firewall-cmd >/dev/null 2>&1 && $SUDO firewall-cmd --state >/dev/null 2>&1; then
  log "Додавання UDP-правил до firewalld..."
  $SUDO firewall-cmd --permanent --add-port="${CHOSEN_PORT}/udp" >/dev/null || true
  $SUDO firewall-cmd --permanent --add-port="5099/udp" >/dev/null || true
  $SUDO firewall-cmd --reload >/dev/null || true
  ok "Правила firewalld налаштовано."
fi

ok "=== Встановлення $APP_NAME завершено успішно! ==="
if [ "$NO_LAUNCH" -eq 0 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x "$PREFIX/bin/lanvid_app" ]; then
  log "Запуск LanVid Studio..."
  nohup "$PREFIX/bin/lanvid_app" >/dev/null 2>&1 &
fi
