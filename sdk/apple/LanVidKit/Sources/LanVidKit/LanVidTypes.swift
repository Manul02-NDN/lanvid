import Foundation
import LanVidCore

public enum LanVidError: Error, LocalizedError {
    case abiMismatch(runtime: String)
    case coreError(code: Int32, message: String)
    case unsupportedPixelFormat(OSType)

    public var errorDescription: String? {
        switch self {
        case .abiMismatch(let rt):
            return "Incompatible LanVidCore ABI (runtime version \(rt))"
        case .coreError(let code, let msg):
            return "LanVid error (\(code)): \(msg)"
        case .unsupportedPixelFormat(let pf):
            return "Unsupported CVPixelBuffer format: \(pf)"
        }
    }
}

public struct LanVidPeer: Identifiable, Hashable, Sendable {
    public var id: String { "\(ip):\(port)" }
    public let ip: String
    public let port: UInt16
    public let rttMs: UInt32
    public let name: String
    public let os: String
    public let simd: String
}

public enum LanVidCodec: Int32, Sendable {
    case dct  = 0
    case h264 = 1
    case h265 = 2
}

public struct LanVidStats: Sendable {
    public let state: Int32
    public let width: Int32
    public let height: Int32
    public let activeQuality: Int32
    public let activeCodec: LanVidCodec
    public let isHardwareAccelerated: Bool
    public let targetBitrateKbps: UInt32
    public let codecBackend: String
    public let fps: Double
    public let bitrateMbps: Double
    public let latencyP50Ms: Double
    public let latencyP99Ms: Double
    public let packetLossPct: Double
    public let fecRecovered: UInt64
    public let audioKbps: Double
    public let audioBufferMs: Double

    internal init(c: LV_StreamStats) {
        self.state = c.state
        self.width = c.width
        self.height = c.height
        self.activeQuality = c.active_quality
        self.activeCodec = LanVidCodec(rawValue: c.active_codec) ?? .dct
        self.isHardwareAccelerated = c.codec_is_hardware != 0
        self.targetBitrateKbps = c.target_bitrate_kbps
        self.codecBackend = cStringFromTuple(c.codec_backend)
        self.fps = c.fps
        self.bitrateMbps = c.bitrate_mbps
        self.latencyP50Ms = c.latency_p50_ms
        self.latencyP99Ms = c.latency_p99_ms
        self.packetLossPct = c.packet_loss_pct
        self.fecRecovered = c.fec_recovered
        self.audioKbps = c.audio_kbps
        self.audioBufferMs = c.audio_buffer_ms
    }
}

public enum LanVidRuntime {
    private static let initialized: Bool = {
        lv_init()
        return true
    }()

    public static func ensureInitialized() throws {
        _ = initialized
        if lv_check_abi_compatibility(LV_SDK_VERSION_MAJOR, LV_SDK_VERSION_MINOR) == 0 {
            throw LanVidError.abiMismatch(runtime: version)
        }
    }

    public static var version: String { String(cString: lv_get_version_string()) }
    public static var simdInfo: String { String(cString: lv_get_cpu_simd_info()) }
    public static var supportedCodecsJson: String {
        guard let p = lv_get_supported_codecs() else { return "[]" }
        defer { lv_free_string(p) }
        return String(cString: p)
    }
}

internal func cStringFromTuple<T>(_ tuple: T) -> String {
    var copy = tuple
    return withUnsafePointer(to: &copy) {
        $0.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) {
            String(cString: $0)
        }
    }
}

internal func writeCString<T>(_ str: String, into tuple: inout T) {
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
