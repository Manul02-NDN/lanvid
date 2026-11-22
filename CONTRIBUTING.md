# Contributing to LanVid

Thank you for your interest in contributing to **LanVid**! We welcome bug reports, SIMD/codec optimizations, platform backend improvements, SDK bindings, and documentation updates.

---

## Local Development Setup

### Build & Run All 31 Unit Tests
**Windows (MSVC 2022 + Ninja):**
```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\lanvid_tests.exe
.\build\bench_codec.exe
```

**Linux (Ubuntu/Debian/Arch/Fedora):**
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
./build/lanvid_tests
./build/bench_codec
```

**macOS (Apple Silicon / Intel):**
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(sysctl -n hw.ncpu)"
./build/lanvid_tests
```

### Verify the Standalone SDK & External CMake Client
```bash
python3 sdk/build_sdk.py --verify-client
```

---

## Coding & Architecture Standards

1. **C++20 Core (`src/`):**
   - Zero third-party dependencies inside `lanvid_core` (`LANVID_SDK_CORE_ONLY=ON` must compile cleanly on Windows, Linux, macOS, iOS, and Android NDK).
   - Keep zero-warning cleanliness under MSVC `/W3 /permissive-` and GCC/Clang `-Wall -Wextra`.
   - Maintain strict slice independence (no cross-slice state or inter-frame dependencies).
2. **SIMD Parity (AVX2 & ARM64 NEON):**
   - Any change to `src/codec/color*.cpp` or `src/codec/dct*.cpp` **must preserve bitstream and visual parity** against the scalar reference (`codec_avx2_vs_scalar_parity` requires `|ΔPSNR_Y| < 0.1 dB` at 4K `3840×2160`).
   - Keep runtime CPU dispatch intact (`--no-simd` / `lv_set_simd_enabled(0)` must always fall back cleanly to scalar).
3. **C ABI Stability (`sdk/include/lanvid/`):**
   - Public headers in `sdk/include/lanvid/` must remain pure C99/C11 with fixed-width integer types and no STL types crossing DLL/SO boundaries.
   - Bump `LV_SDK_VERSION_MINOR` on backwards-compatible additions and `LV_SDK_VERSION_MAJOR` on breaking struct/signature changes.
4. **Network Safety:**
   - All UDP packet parsers (`src/net/packet.cpp`, `assembler.cpp`, `slice_format.cpp`, `discovery.cpp`) must strictly validate payload lengths and bounds on untrusted datagrams.
5. **Line Endings:**
   - Keep `.sh`, `.command`, `preinstall`, and `postinstall` scripts with Unix `LF` endings (enforced by `.gitattributes`).

---

## Pull Request Checklist

- [ ] All **31 / 31 unit tests** in `lanvid_tests` pass (`0 failed`).
- [ ] New features or bug fixes include targeted tests in `tests/`.
- [ ] `python3 sdk/build_sdk.py --verify-client` compiles and passes if C ABI or CMake packaging changed.
- [ ] 4K60 codec performance (`bench_codec`) has no regressions (`< 1.5 ms` encode/decode on modern multi-core AVX2/NEON CPUs).
- [ ] Documentation (`README.md`, `sdk/README.md`, or `docs/PROTOCOL.md`) updated where applicable.
