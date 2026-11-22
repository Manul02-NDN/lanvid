import Foundation
import LanVidCore

/// Async LAN discovery helper returning discovered LanVid receivers over Wi-Fi / Ethernet.
public enum LanVidDiscovery {
    /// Performs a single LAN scan asynchronously without blocking the main actor.
    public static func scanOnce(timeoutMs: Int32 = 400, discoveryPort: UInt16 = 0) async -> [LanVidPeer] {
        try? LanVidRuntime.ensureInitialized()
        return await withCheckedContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                var raw = [LV_DiscoveredPeer](repeating: LV_DiscoveredPeer(), count: 32)
                let count = lv_discovery_scan_peers(timeoutMs, discoveryPort, &raw, 32)
                let peers = (0..<max(0, Int(count))).map { i -> LanVidPeer in
                    let p = raw[i]
                    return LanVidPeer(
                        ip: cStringFromTuple(p.ip),
                        port: p.port,
                        rttMs: p.rtt_ms,
                        name: cStringFromTuple(p.name),
                        os: cStringFromTuple(p.os),
                        simd: cStringFromTuple(p.simd)
                    )
                }
                cont.resume(returning: peers)
            }
        }
    }

    /// Periodic AsyncStream emitting updated peer lists every `intervalSeconds`.
    public static func scan(intervalSeconds: Double = 2.0, timeoutMs: Int32 = 400) -> AsyncStream<[LanVidPeer]> {
        AsyncStream { continuation in
            let task = Task {
                while !Task.isCancelled {
                    let peers = await scanOnce(timeoutMs: timeoutMs)
                    continuation.yield(peers)
                    try? await Task.sleep(nanoseconds: UInt64(max(0.2, intervalSeconds) * 1e9))
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
