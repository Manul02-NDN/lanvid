import Foundation
import SwiftUI

enum ProTallyState: Int, CaseIterable, Codable {
    case standby = 0
    case preview = 1
    case program = 2

    var badgeText: String {
        switch self {
        case .program: return tr("PGM • В ЕФІРІ", "PGM • ON AIR")
        case .preview: return tr("PVW • ПРЕВ'Ю", "PVW • PREVIEW")
        case .standby: return "STBY"
        }
    }

    var accentColor: Color {
        switch self {
        case .program: return CineTheme.tallyRed
        case .preview: return CineTheme.tallyGreen
        case .standby: return CineTheme.slate
        }
    }
}

enum CineLens: String, CaseIterable, Identifiable, Codable {
    case ultraWide = "0.5x"
    case wide = "1x"
    case tele = "2x"
    case front = "Front"
    var id: String { rawValue }

    var displayLabel: String {
        switch self {
        case .front: return tr("Фронт", "Front")
        default: return rawValue
        }
    }
}

enum CineResolution: String, CaseIterable, Identifiable, Codable {
    case uhd4k = "4K"
    case fhd1080 = "1080p"
    case hd720 = "720p"
    var id: String { rawValue }

    var width: Int32 {
        switch self {
        case .uhd4k: return 3840
        case .fhd1080: return 1920
        case .hd720: return 1280
        }
    }

    var height: Int32 {
        switch self {
        case .uhd4k: return 2160
        case .fhd1080: return 1080
        case .hd720: return 720
        }
    }
}

enum CineCodec: String, CaseIterable, Identifiable, Codable {
    case hevc = "H.265 (HEVC)"
    case h264 = "H.264 (AVC)"
    case dct = "LanVid DCT"
    var id: String { rawValue }

    var shortName: String {
        switch self {
        case .hevc: return "H.265"
        case .h264: return "H.264"
        case .dct: return "DCT"
        }
    }

    var lvCodecValue: Int32 {
        switch self {
        case .dct: return 0
        case .h264: return 1
        case .hevc: return 2
        }
    }
}

/// Three adaptive iPhone Duo working modes required for single-phone, multi-cam, and paired dual-iPhone workflows.
enum DuoMode: String, CaseIterable, Identifiable, Codable {
    case singleHUD = "single"
    case dualCamPIP = "dual_pip"
    case dualCamSplit = "dual_split"
    case duoCompanion = "duo_companion"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .singleHUD: return tr("Стандартний HUD", "Single Cine HUD")
        case .dualCamPIP: return tr("Dual-Cam PIP", "Dual-Cam PIP")
        case .dualCamSplit: return tr("Dual-Cam 50/50", "Dual-Cam 50/50 Split")
        case .duoCompanion: return tr("iPhone Duo Пульт", "iPhone Duo Companion")
        }
    }

    var subtitle: String {
        switch self {
        case .singleHUD:
            return tr("Адаптивний кіновидошукач для будь-якого iPhone", "Adaptive cinema HUD for compact & Pro Max iPhones")
        case .dualCamPIP:
            return tr("Основна + фронтальна камера у віконці (MultiCam)", "Back + Front cameras Picture-in-Picture (MultiCam)")
        case .dualCamSplit:
            return tr("Розділений екран 50/50 з окремою експозицією", "50/50 split dual-camera capture with independent exposure")
        case .duoCompanion:
            return tr("Другий iPhone як пульт фокусу, Tally та інтеркому", "Second iPhone as wireless focus, Tally & intercom desk")
        }
    }

    var isMultiCam: Bool {
        self == .dualCamPIP || self == .dualCamSplit
    }
}

enum DuoRole: String, CaseIterable, Identifiable, Codable {
    case camA = "CAM A"
    case camB = "CAM B"
    case companion = "COMPANION"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .camA: return "CAM A"
        case .camB: return "CAM B"
        case .companion: return tr("Режисер / Пульт", "Director Companion")
        }
    }
}

struct DiscoveredStudio: Identifiable, Equatable {
    var id: String { "\(host):\(videoPort)" }
    let name: String
    let host: String
    let videoPort: UInt16
    let tallyPort: UInt16
}

struct DuoPeerCamera: Identifiable, Equatable {
    var id: String { host }
    let host: String
    var name: String
    var role: DuoRole
    var tally: ProTallyState
    var timecode: String
    var focusPosition: Float
    var isoValue: Float
    var shutterAngle: Int
    var wbKelvin: Float
    var lastSeen: Date
}

struct CameraTelemetry {
    var timecode: String = "01:00:00:00"
    var bitrateMbps: Double = 35.0
    var latencyMs: Double = 8.4
    var actualFps: Double = 60.0
    var codecBackend: String = "VideoToolbox HW"
    var micPeakDbfs: Float = -24.0
    var ifbPeakDbfs: Float = -32.0
    var batteryPct: Int = 92
    var tempC: Int = 34
    var histogram: [Float] = [
        0.12, 0.22, 0.35, 0.48, 0.62, 0.74, 0.85, 0.78,
        0.65, 0.58, 0.68, 0.82, 0.90, 0.72, 0.50, 0.34,
        0.26, 0.31, 0.44, 0.38, 0.22, 0.14, 0.08, 0.05
    ]
}
