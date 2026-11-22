# Security Policy

## Supported Versions

| Version | Supported |
|---|---|
| `2.1.x` (Core & Apps) / `SDK 1.0.x` | :white_check_mark: Yes |
| `< 2.0.0` | :x: No |

---

## Security Model & Threat Surface

LanVid is designed for **trusted Local Area Networks (LAN / Wi-Fi)**. Because the receiver binds raw UDP sockets (default video/audio port `5000` and LAN discovery port `5099`) and parses binary datagrams in real time, memory safety and bounds validation in the following components are treated as critical security boundaries:

- **UDP Packet & Header Parser** (`src/net/packet.cpp`): magic/version validation, payload length checks, CRC32C verification.
- **Slice Reassembler & XOR FEC Recovery** (`src/net/assembler.cpp`, `src/net/packetizer.cpp`): fragment index/count bounds, frame-slot caps, parity group length validation.
- **Entropy & Huffman Bitstream Decoder** (`src/codec/entropy.cpp`, `src/codec/slice_codec.cpp`, `src/common/bitio.hpp`): overrun detection on malformed bitstreams, safe abort without out-of-bounds writes.
- **LAN Auto-Discovery** (`src/net/discovery.cpp`): strict length limits and field sanitization on broadcast probes/responses.

> **Recommendation:** Do not expose raw unencrypted LVSP UDP ports directly to the public internet. For WAN streaming, tunnel LanVid over WireGuard or another authenticated VPN.

---

## Reporting a Vulnerability

If you discover a security vulnerability (such as an out-of-bounds read/write, crash/DoS via crafted UDP packets, privilege escalation in an installer script, or memory corruption in JNI/C ABI):

1. **Do NOT open a public GitHub Issue** with exploit details.
2. Use **GitHub Private Vulnerability Reporting** (*Security → Advisories → Report a vulnerability*) on this repository.
3. Include:
   - Affected component/file and version/commit hash.
   - Minimal reproduction steps or malformed packet hex dump / unit test.
   - Impact assessment (crash, memory corruption, privilege escalation, etc.).

We aim to acknowledge reports within **48 hours** and release a coordinated fix with a regression test in `tests/`.
