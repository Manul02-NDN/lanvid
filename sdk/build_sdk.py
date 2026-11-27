"""
LanVid Video (v3.0.0) & CrossLink Audio (v1.0.0) Unified Cross-Platform SDK Packager

Supports selectable protocol installation / packaging:
  --protocol all     Both LanVid Video SDK + CrossLink Audio SDK (default)
  --protocol video   LanVid Video + 2ch Stereo Audio SDK only
  --protocol audio   CrossLink 64ch AoIP + MIDI 2.0 + Monitor + Grid SDK only

Usage:
  python3 sdk/build_sdk.py [--protocol all|video|audio] [--verify-client] [--no-macos-pkg]
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LANVID_VERSION = "3.0.0"
CROSSLINK_VERSION = "1.0.0"


def dist_dir_for(protocol: str) -> Path:
    if protocol == "video":
        return ROOT / "dist" / f"LanVidSDK-v{LANVID_VERSION}"
    if protocol == "audio":
        return ROOT / "dist" / f"CrossLinkSDK-v{CROSSLINK_VERSION}"
    return ROOT / "dist" / f"LanVidCrossLinkSDK-v{LANVID_VERSION}"


def run(cmd: list[str], cwd: Path | None = None, shell: bool = False) -> None:
    print(">>", " ".join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True, shell=shell)


def run_cmake_windows(args_str: str) -> None:
    cmake_exe = shutil.which("cmake") or r"C:\Program Files\CMake\bin\cmake.exe"
    if shutil.which("cl"):
        subprocess.run(f'"{cmake_exe}" {args_str}', shell=True, check=True)
        return
    candidates = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    ]
    vcvars = next((p for p in candidates if Path(p).is_file()), None)
    if vcvars:
        subprocess.run(f'call "{vcvars}" >nul && "{cmake_exe}" {args_str}', shell=True, check=True)
    else:
        subprocess.run(f'"{cmake_exe}" {args_str}', check=True, shell=True)


def cmake_generator_args() -> list[str]:
    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    if platform.system() != "Windows":
        return ["-G", "Unix Makefiles"]
    return []


def build_host_sdk(protocol: str, dist_dir: Path, build_dir: Path) -> None:
    if build_dir.exists():
        shutil.rmtree(build_dir)
    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    dist_dir.mkdir(parents=True, exist_ok=True)

    comp = protocol.upper()
    gen_args = cmake_generator_args()
    gen_str = ("-G " + gen_args[1] + " ") if len(gen_args) == 2 else ""
    cfg_flags = (
        f'-S "{ROOT}" -B "{build_dir}" {gen_str}'
        f'-DCMAKE_BUILD_TYPE=Release '
        f'-DLANVID_SDK_COMPONENTS={comp} '
        f'-DLANVID_SDK_CORE_ONLY=ON '
        f'-DLANVID_BUILD_SHARED=ON '
        f'-DLANVID_BUILD_TESTS=OFF'
    )
    if platform.system() == "Windows":
        run_cmake_windows(cfg_flags)
        run_cmake_windows(f'--build "{build_dir}" --config Release')
        run_cmake_windows(f'--install "{build_dir}" --config Release --prefix "{dist_dir}"')
    else:
        cmake_exe = shutil.which("cmake") or "cmake"
        run([
            cmake_exe, "-S", str(ROOT), "-B", str(build_dir), *gen_args,
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DLANVID_SDK_COMPONENTS={comp}",
            "-DLANVID_SDK_CORE_ONLY=ON",
            "-DLANVID_BUILD_SHARED=ON",
            "-DLANVID_BUILD_TESTS=OFF",
        ])
        run([cmake_exe, "--build", str(build_dir)])
        run([cmake_exe, "--install", str(build_dir), "--prefix", str(dist_dir)])


def copy_tree_clean(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    ignore = {".gradle", ".kotlin", "local.properties", "build", ".cxx", "__pycache__"}
    shutil.copytree(src, dst, ignore=lambda _d, names: [n for n in names if n in ignore])


def assemble_sdk_bundle(protocol: str, dist_dir: Path) -> None:
    want_video = protocol in ("all", "video")
    want_audio = protocol in ("all", "audio")

    (dist_dir / "python").mkdir(parents=True, exist_ok=True)
    (dist_dir / "cpp").mkdir(parents=True, exist_ok=True)
    (dist_dir / "examples").mkdir(parents=True, exist_ok=True)
    (dist_dir / "apple").mkdir(parents=True, exist_ok=True)
    (dist_dir / "android").mkdir(parents=True, exist_ok=True)

    if want_video:
        shutil.copy2(ROOT / "sdk" / "python" / "lanvid_sdk.py", dist_dir / "python" / "lanvid_sdk.py")
        shutil.copy2(ROOT / "sdk" / "cpp" / "lanvid.hpp", dist_dir / "cpp" / "lanvid.hpp")
        copy_tree_clean(ROOT / "sdk" / "examples" / "cpp_video_client", dist_dir / "examples" / "cpp_video_client")
        copy_tree_clean(ROOT / "sdk" / "apple" / "LanVidKit", dist_dir / "apple" / "LanVidKit")
        copy_tree_clean(ROOT / "sdk" / "android" / "lanvid-sdk", dist_dir / "android" / "lanvid-sdk")
        aar_v = ROOT / "dist" / "android" / f"lanvid-video-sdk-{LANVID_VERSION}.aar"
        if aar_v.is_file():
            shutil.copy2(aar_v, dist_dir / "android" / aar_v.name)
        xcf_v = ROOT / "dist" / "apple" / "LanVidCore.xcframework"
        if xcf_v.is_dir():
            copy_tree_clean(xcf_v, dist_dir / "apple" / "LanVidCore.xcframework")

    if want_audio:
        shutil.copy2(ROOT / "sdk" / "python" / "crosslink_sdk.py", dist_dir / "python" / "crosslink_sdk.py")
        shutil.copy2(ROOT / "sdk" / "cpp" / "crosslink.hpp", dist_dir / "cpp" / "crosslink.hpp")
        copy_tree_clean(ROOT / "sdk" / "examples" / "cpp_audio_client", dist_dir / "examples" / "cpp_audio_client")
        copy_tree_clean(ROOT / "sdk" / "apple" / "CrossLinkKit", dist_dir / "apple" / "CrossLinkKit")
        copy_tree_clean(ROOT / "sdk" / "android" / "crosslink-sdk", dist_dir / "android" / "crosslink-sdk")
        aar_a = ROOT / "dist" / "android" / f"crosslink-audio-sdk-{CROSSLINK_VERSION}.aar"
        if aar_a.is_file():
            shutil.copy2(aar_a, dist_dir / "android" / aar_a.name)
        xcf_a = ROOT / "dist" / "apple" / "CrossLinkCore.xcframework"
        if xcf_a.is_dir():
            copy_tree_clean(xcf_a, dist_dir / "apple" / "CrossLinkCore.xcframework")

    if want_video and want_audio:
        copy_tree_clean(ROOT / "sdk" / "examples" / "cpp_combined_client", dist_dir / "examples" / "cpp_combined_client")
        pkg_swift = ROOT / "sdk" / "apple" / "Package.swift"
        if pkg_swift.is_file():
            (dist_dir / "apple" / "Package.swift").write_text(
                pkg_swift.read_text(encoding="utf-8")
                .replace("../../dist/apple/LanVidCore.xcframework", "LanVidCore.xcframework")
                .replace("../../dist/apple/CrossLinkCore.xcframework", "CrossLinkCore.xcframework"),
                encoding="utf-8",
            )
        for apple_art in [
            f"LanVidCrossLinkSDK-v{LANVID_VERSION}-macOS.pkg",
            f"LanVidCrossLinkSDK-v{LANVID_VERSION}-macOS.dmg",
        ]:
            src_art = ROOT / "dist" / "apple" / apple_art
            if src_art.is_file():
                shutil.copy2(src_art, dist_dir / "apple" / apple_art)

    shutil.copy2(ROOT / "sdk" / "README.md", dist_dir / "README.md")
    print(f"Assembled SDK distribution [{protocol}] at: {dist_dir}")


def _run_client_example(example_dir: Path, exe_names: list[str], dist_dir: Path) -> None:
    client_build = ROOT / f"build-sdk-client-{example_dir.name}"
    if client_build.exists():
        shutil.rmtree(client_build)
    gen_args = cmake_generator_args()
    gen_str = ("-G " + gen_args[1] + " ") if len(gen_args) == 2 else ""
    if platform.system() == "Windows":
        run_cmake_windows(
            f'-S "{example_dir}" -B "{client_build}" {gen_str}'
            f'-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="{dist_dir}"'
        )
        run_cmake_windows(f'--build "{client_build}" --config Release')
        for name in exe_names:
            exe = client_build / f"{name}.exe"
            if not exe.is_file():
                exe = client_build / "Release" / f"{name}.exe"
            run([str(exe)])
    else:
        cmake_exe = shutil.which("cmake") or "cmake"
        run([
            cmake_exe, "-S", str(example_dir), "-B", str(client_build), *gen_args,
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={dist_dir}",
        ])
        run([cmake_exe, "--build", str(client_build)])
        for name in exe_names:
            run([str(client_build / name)])
    shutil.rmtree(client_build, ignore_errors=True)


def verify_external_cpp_clients(protocol: str, dist_dir: Path) -> None:
    if protocol in ("all", "video"):
        _run_client_example(dist_dir / "examples" / "cpp_video_client", ["lanvid_video_app"], dist_dir)
    if protocol in ("all", "audio"):
        _run_client_example(dist_dir / "examples" / "cpp_audio_client", ["crosslink_audio_app"], dist_dir)
    if protocol == "all":
        _run_client_example(
            dist_dir / "examples" / "cpp_combined_client",
            ["combined_shared", "combined_static"],
            dist_dir,
        )


def main() -> None:
    ap = argparse.ArgumentParser(description="Build & package LanVid Video + CrossLink Audio SDK")
    ap.add_argument("--protocol", choices=["all", "video", "audio"], default="all",
                    help="Protocol component to build: all (default), video (LanVid), audio (CrossLink)")
    ap.add_argument("--build-apple", action="store_true", help="Build Apple XCFramework(s) on macOS")
    ap.add_argument("--no-macos-pkg", action="store_true", help="Skip macOS .pkg/.dmg generation on darwin")
    ap.add_argument("--verify-client", action="store_true", help="Compile & run external find_package C++ client(s)")
    args = ap.parse_args()

    dist_dir = dist_dir_for(args.protocol)
    build_dir = ROOT / f"build-sdk-{args.protocol}"

    build_host_sdk(args.protocol, dist_dir, build_dir)
    if sys.platform == "darwin":
        if not args.no_macos_pkg and args.protocol == "all":
            run(["bash", str(ROOT / "sdk" / "apple" / "build_macos_pkg.sh")])
        elif args.build_apple:
            run(["bash", str(ROOT / "sdk" / "apple" / "build_xcframework.sh"), "--protocol", args.protocol])
    assemble_sdk_bundle(args.protocol, dist_dir)
    if args.verify_client:
        verify_external_cpp_clients(args.protocol, dist_dir)


if __name__ == "__main__":
    main()
