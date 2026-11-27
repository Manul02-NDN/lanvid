"""
LanVidSDK Unified Cross-Platform Packager (sdk/build_sdk.py)

Builds and installs a clean SDK distribution under `dist/LanVidSDK-v2.7.0/`:
  - include/lanvid/       (C ABI headers + C++17 RAII lanvid.hpp)
  - bin/ & lib/           (lanvid_core.dll / liblanvid_core.so / liblanvid_core.dylib + static lib)
  - lib/cmake/LanVidSDK/  (LanVidSDKConfig.cmake, LanVidSDKTargets.cmake)
  - lib/pkgconfig/        (lanvid-sdk.pc)
  - apple/                (SwiftPM LanVidKit + XCFramework build script + SwiftUI examples)
  - android/              (lanvid-sdk-2.7.0.aar + Gradle library module)
  - cpp/                  (lanvid.hpp + find_package CMake consumer example)
  - python/               (lanvid_sdk.py ctypes + NumPy binding)
  - README.md

Usage:
  python sdk/build_sdk.py [--verify-client] [--build-android] [--build-apple]
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK_VERSION = "2.7.0"
DIST_DIR = ROOT / "dist" / f"LanVidSDK-v{SDK_VERSION}"
BUILD_SDK_DIR = ROOT / "build-sdk"


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
        cmd = f'call "{vcvars}" >nul && "{cmake_exe}" {args_str}'
        subprocess.run(cmd, shell=True, check=True)
    else:
        subprocess.run(f'"{cmake_exe}" {args_str}', check=True, shell=True)


def cmake_generator_args() -> list[str]:
    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    if platform.system() != "Windows":
        return ["-G", "Unix Makefiles"]
    return []


def build_host_sdk() -> None:
    if BUILD_SDK_DIR.exists():
        shutil.rmtree(BUILD_SDK_DIR)
    if DIST_DIR.exists():
        shutil.rmtree(DIST_DIR)
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    gen_args = cmake_generator_args()
    gen_str = ("-G " + gen_args[1] + " ") if len(gen_args) == 2 else ""
    cfg_flags = (
        f'-S "{ROOT}" -B "{BUILD_SDK_DIR}" {gen_str}'
        f'-DCMAKE_BUILD_TYPE=Release '
        f'-DLANVID_SDK_CORE_ONLY=ON '
        f'-DLANVID_BUILD_SHARED=ON '
        f'-DLANVID_BUILD_TESTS=OFF'
    )
    if platform.system() == "Windows":
        run_cmake_windows(cfg_flags)
        run_cmake_windows(f'--build "{BUILD_SDK_DIR}" --config Release')
        run_cmake_windows(f'--install "{BUILD_SDK_DIR}" --config Release --prefix "{DIST_DIR}"')
    else:
        cmake_exe = shutil.which("cmake") or "cmake"
        run([cmake_exe, "-S", str(ROOT), "-B", str(BUILD_SDK_DIR), *gen_args,
             "-DCMAKE_BUILD_TYPE=Release", "-DLANVID_SDK_CORE_ONLY=ON",
             "-DLANVID_BUILD_SHARED=ON", "-DLANVID_BUILD_TESTS=OFF"])
        run([cmake_exe, "--build", str(BUILD_SDK_DIR)])
        run([cmake_exe, "--install", str(BUILD_SDK_DIR), "--prefix", str(DIST_DIR)])


def copy_tree_clean(src: Path, dst: Path, ignore_names: set[str] | None = None) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    ignore = ignore_names or {".gradle", ".kotlin", "local.properties", "build", ".cxx", "__pycache__"}
    shutil.copytree(src, dst, ignore=lambda _d, names: [n for n in names if n in ignore])


def assemble_sdk_bundle() -> None:
    # Copy language bindings & platform packages into dist/LanVidSDK-v2.7.0/
    copy_tree_clean(ROOT / "sdk" / "apple", DIST_DIR / "apple")
    copy_tree_clean(ROOT / "sdk" / "cpp", DIST_DIR / "cpp")
    copy_tree_clean(ROOT / "sdk" / "examples", DIST_DIR / "examples")
    copy_tree_clean(ROOT / "sdk" / "python", DIST_DIR / "python")
    copy_tree_clean(ROOT / "sdk" / "android", DIST_DIR / "android")

    # Copy built Android AAR if available
    aar = ROOT / "dist" / "android" / f"lanvid-sdk-{SDK_VERSION}.aar"
    if aar.is_file():
        shutil.copy2(aar, DIST_DIR / "android" / aar.name)

    # Copy built Apple XCFramework, .pkg, and .dmg if available
    xcf = ROOT / "dist" / "apple" / "LanVidCore.xcframework"
    if xcf.is_dir():
        copy_tree_clean(xcf, DIST_DIR / "apple" / "LanVidCore.xcframework")
        pkg_swift = DIST_DIR / "apple" / "Package.swift"
        if pkg_swift.is_file():
            pkg_swift.write_text(
                pkg_swift.read_text(encoding="utf-8").replace(
                    "../../dist/apple/LanVidCore.xcframework",
                    "LanVidCore.xcframework",
                ),
                encoding="utf-8",
            )

    for apple_art in [f"LanVidSDK-v{SDK_VERSION}-macOS.pkg", f"LanVidSDK-v{SDK_VERSION}-macOS.dmg"]:
        src_art = ROOT / "dist" / "apple" / apple_art
        if src_art.is_file():
            shutil.copy2(src_art, DIST_DIR / "apple" / apple_art)

    shutil.copy2(ROOT / "sdk" / "README.md", DIST_DIR / "README.md")
    print(f"Assembled SDK distribution at: {DIST_DIR}")


def verify_external_cpp_client() -> None:
    client_src = DIST_DIR / "examples" / "cpp_client"
    client_build = ROOT / "build-sdk-client-test"
    if client_build.exists():
        shutil.rmtree(client_build)

    gen_args = cmake_generator_args()
    gen_str = ("-G " + gen_args[1] + " ") if len(gen_args) == 2 else ""
    cfg = (
        f'-S "{client_src}" -B "{client_build}" {gen_str}'
        f'-DCMAKE_BUILD_TYPE=Release '
        f'-DCMAKE_PREFIX_PATH="{DIST_DIR}"'
    )
    if platform.system() == "Windows":
        run_cmake_windows(cfg)
        run_cmake_windows(f'--build "{client_build}" --config Release')
        exe = client_build / "my_app.exe"
        if not exe.is_file():
            exe = client_build / "Release" / "my_app.exe"
        run([str(exe)])
    else:
        cmake_exe = shutil.which("cmake") or "cmake"
        run([cmake_exe, "-S", str(client_src), "-B", str(client_build), *gen_args,
             "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={DIST_DIR}"])
        run([cmake_exe, "--build", str(client_build)])
        run([str(client_build / "my_app")])


def main() -> None:
    ap = argparse.ArgumentParser(description="Build and package LanVidSDK")
    ap.add_argument("--build-apple", action="store_true", help="Build Apple XCFramework & macOS .pkg/.dmg (auto on macOS)")
    ap.add_argument("--no-macos-pkg", action="store_true", help="Skip macOS .pkg/.dmg generation on darwin")
    ap.add_argument("--verify-client", action="store_true", help="Compile & run external find_package C++ client")
    args = ap.parse_args()

    build_host_sdk()
    if sys.platform == "darwin":
        if not args.no_macos_pkg:
            run(["bash", str(ROOT / "sdk" / "apple" / "build_macos_pkg.sh")])
        elif args.build_apple:
            run(["bash", str(ROOT / "sdk" / "apple" / "build_xcframework.sh")])
    assemble_sdk_bundle()
    if args.verify_client:
        verify_external_cpp_client()


if __name__ == "__main__":
    main()
