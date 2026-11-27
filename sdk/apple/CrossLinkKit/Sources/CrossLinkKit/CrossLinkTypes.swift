import CrossLinkCore
import Foundation

public enum CrossLinkError: Error, LocalizedError {
    case abiMismatch(runtime: String)
    case coreError(code: Int32, message: String)

    public var errorDescription: String? {
        switch self {
        case .abiMismatch(let rt):
            return "Incompatible CrossLinkCore ABI (runtime version \(rt))"
        case .coreError(let code, let msg):
            return "CrossLink error (\(code)): \(msg)"
        }
    }
}

public struct CrossLinkAudioNode: Identifiable, Hashable, Sendable {
    public var id: String { "\(ip):\(rxPort)" }
    public let ip: String
    public let name: String
    public let hostname: String
    public let audioDriver: String
    public let rxPort: UInt16
    public let txPort: UInt16
    public let channels: UInt16
    public let duplex: Bool
    public let rttMs: UInt32
    public let os: String
}

public struct CrossLinkMidiEvent: Sendable {
    public let mediaTimestampUs: UInt64
    public let eventSeq: UInt32
    public let blockOffsetFrames: Int32
    public let status: UInt8
    public let data1: UInt8
    public let data2: UInt8
    public let port: UInt8

    internal init(c: CL_MidiEvent) {
        self.mediaTimestampUs = c.media_ts_us
        self.eventSeq = c.event_seq
        self.blockOffsetFrames = c.block_offset_frames
        self.status = c.status
        self.data1 = c.data1
        self.data2 = c.data2
        self.port = c.port
    }
}

public struct CrossLinkStreamStats: Sendable {
    public let state: Int32
    public let clockSynced: Bool
    public let rttUs: UInt32
    public let clockSkewPpm: Double
    public let audioKbps: Double
    public let effectiveLatencyMs: Double
    public let effectiveTargetMs: UInt32
    public let adaptiveJitter: Bool
    public let audioPackets: UInt64
    public let audioBytes: UInt64
    public let fecPackets: UInt64
    public let retransmits: UInt64
    public let midiEvents: UInt64
    public let streamEpochResets: UInt64

    internal init(c: CL_StreamStats) {
        self.state = c.state
        self.clockSynced = c.clock_synced != 0
        self.rttUs = c.rtt_us
        self.clockSkewPpm = c.clock_skew_ppm
        self.audioKbps = c.audio_kbps
        self.effectiveLatencyMs = c.effective_latency_ms
        self.effectiveTargetMs = c.effective_target_ms
        self.adaptiveJitter = c.adaptive_jitter != 0
        self.audioPackets = c.audio_packets
        self.audioBytes = c.audio_bytes
        self.fecPackets = c.fec_packets
        self.retransmits = c.retransmits
        self.midiEvents = c.midi_events
        self.streamEpochResets = c.stream_epoch_resets
    }
}

public enum CrossLinkRuntime {
    private static let initialized: Bool = {
        cl_init()
        return true
    }()

    public static func ensureInitialized() throws {
        _ = initialized
        if cl_check_abi_compatibility(CL_SDK_VERSION_MAJOR, CL_SDK_VERSION_MINOR) == 0 {
            throw CrossLinkError.abiMismatch(runtime: version)
        }
    }

    public static var version: String { String(cString: cl_get_version_string()) }
    public static var simdInfo: String { String(cString: cl_get_cpu_simd_info()) }
}

internal func clCStringFromTuple<T>(_ tuple: T) -> String {
    var copy = tuple
    return withUnsafePointer(to: &copy) {
        $0.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) {
            String(cString: $0)
        }
    }
}

internal func clWriteCString<T>(_ str: String, into tuple: inout T) {
    let cap = MemoryLayout<T>.size
    withUnsafeMutablePointer(to: &tuple) {
        $0.withMemoryRebound(to: CChar.self, capacity: cap) { dst in
            dst.initialize(repeating: 0, count: cap)
            _ = str.withCString { src in
                strncpy(dst, src, cap - 1)
            }
        }
    }
}
