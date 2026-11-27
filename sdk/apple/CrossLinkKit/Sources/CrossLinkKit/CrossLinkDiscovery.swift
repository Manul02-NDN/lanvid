import CrossLinkCore
import Foundation

/// Async LAN discovery helper returning discovered CrossLink Audio Nodes over UDP :5021.
public enum CrossLinkDiscovery {
    public static func scanOnce(timeoutMs: Int32 = 400, discoveryPort: UInt16 = 0) async -> [CrossLinkAudioNode] {
        try? CrossLinkRuntime.ensureInitialized()
        return await withCheckedContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                var raw = [CL_AudioNode](repeating: CL_AudioNode(), count: 32)
                let count = cl_discovery_scan_audio_nodes(timeoutMs, discoveryPort, &raw, 32)
                let nodes = (0..<max(0, Int(count))).map { i -> CrossLinkAudioNode in
                    let n = raw[i]
                    return CrossLinkAudioNode(
                        ip: clCStringFromTuple(n.ip),
                        name: clCStringFromTuple(n.name),
                        hostname: clCStringFromTuple(n.hostname),
                        audioDriver: clCStringFromTuple(n.audio_driver),
                        rxPort: n.rx_port,
                        txPort: n.tx_port,
                        channels: n.channels,
                        duplex: n.duplex != 0,
                        rttMs: n.rtt_ms,
                        os: clCStringFromTuple(n.os)
                    )
                }
                cont.resume(returning: nodes)
            }
        }
    }

    public static func scan(intervalSeconds: Double = 2.0, timeoutMs: Int32 = 400) -> AsyncStream<[CrossLinkAudioNode]> {
        AsyncStream { continuation in
            let task = Task {
                while !Task.isCancelled {
                    let nodes = await scanOnce(timeoutMs: timeoutMs)
                    continuation.yield(nodes)
                    try? await Task.sleep(nanoseconds: UInt64(max(0.2, intervalSeconds) * 1e9))
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }
}
