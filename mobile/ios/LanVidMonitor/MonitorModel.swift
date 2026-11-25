import Foundation
import LanVidCore

/// Quality of the stereo monitor stream the PC sends to this phone (per channel, as in LanVid Studio).
enum StreamQuality: Int, CaseIterable, Identifiable, Codable {
    case k128 = 128, k192 = 192, k256 = 256, lossless = 0
    var id: Int { rawValue }
    var label: String { self == .lossless ? "Lossless" : "\(rawValue)" }
    var detail: String {
        self == .lossless ? tr("24-bit PCM ≈ 2.3 Мбіт/с", "24-bit PCM ≈ 2.3 Mbit/s")
                          : tr("\(rawValue) кбіт/с на канал", "\(rawValue) kbit/s per channel")
    }
    /// kHello value: total stereo kbps (2 x per channel) or LV_MONITOR_BITRATE_LOSSLESS.
    var wireKbps: UInt16 { self == .lossless ? UInt16(LV_MONITOR_BITRATE_LOSSLESS) : UInt16(rawValue * 2) }
}

/// Monitor buffer on the phone (Fixed: this is exactly the delay the musician hears).
enum MonitorBuffer: Int, CaseIterable, Identifiable, Codable {
    case ms5 = 5, ms10 = 10, ms15 = 15, ms20 = 20
    var id: Int { rawValue }
}

struct ChannelStrip: Identifiable, Equatable {
    let id: Int               // channel index on the PC (0..63)
    var name: String
    var peakDbfs: Float = -120
    var gainDb: Float = 0     // -60 ... +12
    var pan: Float = 0        // -1 (L) ... +1 (R)
    var mute = false
    var solo = false
}

enum HpfMode: Int, CaseIterable, Identifiable, Codable {
    case off = 0, hz40 = 40, hz80 = 80, hz120 = 120
    var id: Int { rawValue }
    var label: String { self == .off ? "Off" : "\(rawValue) " + tr("Гц", "Hz") }
}

/// Server-side DSP of this phone's personal monitor bus (runs on the PC, PersonalMonitorBus).
struct DspSettings: Equatable, Codable {
    var hpf: HpfMode = .off
    var eqOn = true
    var eqLowDb: Float = 0
    var eqMidDb: Float = 0
    var eqMidHz: Float = 1200
    var eqHighDb: Float = 0
    var compOn = false
    var compThresholdDb: Float = -18
    var compRatio: Float = 3
    var compMakeupDb: Float = 0
    var masterVolumeDb: Float = 0
    var masterMute = false

    static let midFrequencies: [Float] = [300, 600, 1200, 2500, 5000]

    var cParams: LV_MonitorDspParams {
        var p = LV_MonitorDspParams()
        p.hpf_enabled = hpf == .off ? 0 : 1
        p.hpf_hz = Float(hpf == .off ? 80 : hpf.rawValue)
        p.eq_enabled = eqOn ? 1 : 0
        p.eq_low_db = eqLowDb
        p.eq_mid_hz = eqMidHz
        p.eq_mid_db = eqMidDb
        p.eq_high_db = eqHighDb
        p.comp_enabled = compOn ? 1 : 0
        p.comp_thresh_db = compThresholdDb
        p.comp_ratio = compRatio
        p.comp_attack_ms = 5
        p.comp_release_ms = 80
        p.comp_makeup_db = compMakeupDb
        p.master_volume_db = masterVolumeDb
        p.master_mute = masterMute ? 1 : 0
        return p
    }
}

/// A LanVid Studio found on the LAN (mDNS "_lanvid._udp") or typed in.
struct StudioEndpoint: Identifiable, Hashable {
    var id: String { "\(host):\(port)" }
    var name: String
    var host: String
    var port: UInt16
}

struct MonitorTelemetry: Equatable {
    var gainReductionDb: Float = 0
    var mixPeakDbfs: Float = -120
    var bufferMs: Double = 0
    var audioPackets: UInt64 = 0
    var receiving = false
    var clockLocked = false
    var clockOffsetJitterUs: UInt32 = 0
    var duplicatesRecovered: UInt64 = 0
    var activeFlowChannels: UInt32 = 0
    var effectivePlayoutMs: Double = 0
    var wifiRedundancyEnabled = false
}

/// Active physical network adapter on iPhone/iPad (Wi-Fi en0, USB-C Ethernet en2, ...).
struct NetInterfaceOption: Identifiable, Hashable {
    var id: String
    var systemName: String
    var friendlyName: String
    var typeTag: String
    var ipv4: String
    var speedMbps: UInt32
    var isUp: Bool

    var displayLabel: String {
        let spd = speedMbps > 0 ? " (\(speedMbps) Mbps)" : ""
        return "[\(typeTag)] \(friendlyName) — \(ipv4)\(spd)"
    }
}
