"""Generates universal self-extracting installer/linux/LanVid-Linux-x86_64.run
with embedded clean tar.gz payload (install_lanvid.sh + .desktop + .svg + full C++20/ImGui sources)."""
import io
import os
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINUX_DIR = ROOT / "installer" / "linux"
OUT_RUN = LINUX_DIR / "LanVid-Linux-x86_64.run"

INCLUDE_DIRS = ["cmake", "include", "src", "third_party/imgui"]
INCLUDE_FILES = ["CMakeLists.txt"]

HEADER = (
    "#!/usr/bin/env bash\n"
    "set -euo pipefail\n"
    "TMPDIR=\"$(mktemp -d /tmp/lanvid-install.XXXXXX)\"\n"
    "trap 'rm -rf \"$TMPDIR\"' EXIT\n"
    "ARCHIVE_LINE=\"$(awk '/^__LANVID_ARCHIVE_BELOW__$/ {print NR + 1; exit 0;}' \"$0\")\"\n"
    "tail -n +\"$ARCHIVE_LINE\" \"$0\" | tar -xz -C \"$TMPDIR\"\n"
    "exec bash \"$TMPDIR/install_lanvid.sh\" \"$@\"\n"
    "__LANVID_ARCHIVE_BELOW__\n"
).encode("utf-8")


def add_bytes(tf: tarfile.TarFile, arcname: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo(name=arcname)
    ti.size = len(data)
    ti.mode = mode
    tf.addfile(ti, io.BytesIO(data))


def main() -> None:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", compresslevel=9) as tf:
        # Installer scripts & desktop metadata (normalize CRLF -> LF for Linux!)
        for fname, mode in [("install_lanvid.sh", 0o755), ("lanvid.desktop", 0o644), ("lanvid.svg", 0o644)]:
            raw = (LINUX_DIR / fname).read_bytes().replace(b"\r\n", b"\n")
            add_bytes(tf, fname, raw, mode)

        # Core & ImGui source bundle so Linux installer builds natively on Ubuntu/Arch/Fedora/Mint
        for rel in INCLUDE_FILES:
            p = ROOT / rel
            add_bytes(tf, f"src_bundle/{rel}", p.read_bytes().replace(b"\r\n", b"\n"), 0o644)

        for rel_dir in INCLUDE_DIRS:
            base = ROOT / rel_dir
            for path in sorted(base.rglob("*")):
                if not path.is_file():
                    continue
                arc = "src_bundle/" + path.relative_to(ROOT).as_posix()
                data = path.read_bytes()
                if path.suffix in {".h", ".hpp", ".c", ".cpp", ".cmake", ".txt", ".sh"}:
                    data = data.replace(b"\r\n", b"\n")
                add_bytes(tf, arc, data, 0o644)

    payload = buf.getvalue()
    OUT_RUN.write_bytes(HEADER + payload)
    print(f"Built {OUT_RUN} ({len(HEADER) + len(payload):,} bytes)")


if __name__ == "__main__":
    main()
