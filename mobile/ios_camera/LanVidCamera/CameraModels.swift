import Foundation
import SwiftUI

enum ProTallyState: Int, CaseIterable, Codable {
    case standby = 0
    case preview = 1
    case program = 2

    var badgeText: String {
        switch self {
        case .program: return tr("В ефірі", "ON AIR")
        case .preview: return tr("Прев'ю", "PREVIEW")
        case .standby: return tr("Очікування", "STANDBY")
        }
    }

    var shortCode: String {
        switch self {
        case .program: return "PGM"
        case .preview: return "PVW"
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
    case tele3x = "3x"
    case front = "Front"
    var id: String { rawValue }

    var displayLabel: String {
        switch self {
        case .front: return tr("Фронт", "Front")
        default: return rawValue
        }
    }

    var targetDisplayZoom: CGFloat {
        switch self {
        case .ultraWide: return 0.5
        case .wide: return 1.0
        case .tele: return 2.0
        case .tele3x: return 3.0
        case .front: return 1.0
        }
    }
}

/// Multi-level hardware video stabilization (AVFoundation AVCaptureVideoStabilizationMode).
enum StabilizationLevel: String, CaseIterable, Identifiable, Codable {
    case off = "off"
    case standard = "standard"
    case cinematic = "cinematic"
    case active = "active"
    case ultra = "ultra"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .off: return tr("Вимк.", "Off")
        case .standard: return tr("Стандарт", "Standard")
        case .cinematic: return tr("Кіно", "Cinematic")
        case .active: return tr("Активна", "Active")
        case .ultra: return tr("Макс", "Ultra")
        }
    }

    var shortBadge: String {
        switch self {
        case .off: return tr("EIS Вимк", "EIS Off")
        case .standard: return tr("EIS Станд", "EIS Std")
        case .cinematic: return tr("EIS Кіно", "EIS Cine")
        case .active: return tr("EIS Актив", "EIS Act")
        case .ultra: return tr("EIS Макс", "EIS Max")
        }
    }

    var subtitle: String {
        switch self {
        case .off:
            return tr("Без кропу · 0 мс буфера · штатив / стабілізатор", "Zero crop · 0 ms buffer · tripod / gimbal")
        case .standard:
            return tr("Оптична + легка EIS · ~5% кроп · ~16 мс", "Optical + light EIS · ~5% crop · ~16 ms")
        case .cinematic:
            return tr("Плавне кінодемпфування · ~10% кроп · ~33 мс", "Smooth cinema damping · ~10% crop · ~33 ms")
        case .active:
            return tr("Для ходьби та динаміки · ~15% кроп · ~50 мс", "Walking & handheld motion · ~15% crop · ~50 ms")
        case .ultra:
            return tr("Максимальна екшн-стабілізація · ~20% кроп · ~66 мс", "Maximum action stabilization · ~20% crop · ~66 ms")
        }
    }

    var systemIcon: String {
        switch self {
        case .off: return "nosign"
        case .standard: return "gyroscope"
        case .cinematic: return "film"
        case .active: return "figure.walk"
        case .ultra: return "bolt.fill"
        }
    }

    var cropPercent: Int {
        switch self {
        case .off: return 0
        case .standard: return 5
        case .cinematic: return 10
        case .active: return 15
        case .ultra: return 20
        }
    }

    var latencyHintMs: Int {
        switch self {
        case .off: return 0
        case .standard: return 16
        case .cinematic: return 33
        case .active: return 50
        case .ultra: return 66
        }
    }

    var levelProgress: Double {
        switch self {
        case .off: return 0.0
        case .standard: return 0.28
        case .cinematic: return 0.54
        case .active: return 0.78
        case .ultra: return 1.0
        }
    }
}

/// Active drawer panel in the LanVid Studio bottom dock.
enum StudioDrawerTab: String, CaseIterable, Identifiable {
    case camera
    case quality
    case monitor
    case devices

    var id: String { rawValue }

    var title: String {
        switch self {
        case .camera: return tr("Камера", "Camera")
        case .quality: return tr("Якість", "Quality")
        case .monitor: return tr("Монітор", "Monitor")
        case .devices: return tr("Мережа", "Devices")
        }
    }

    var icon: String {
        switch self {
        case .camera: return "camera.aperture"
        case .quality: return "slider.horizontal.3"
        case .monitor: return "waveform.path.ecg"
        case .devices: return "desktopcomputer"
        }
    }
}

enum CineResolution: String, CaseIterable, Identifiable, Codable {
    case hd720 = "720p"
    case fhd1080 = "1080p"
    case uhd4k = "4K"
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

struct TelemetrySample: Identifiable {
    let id = UUID()
    let bitrateMbps: Double
    let latencyMs: Double
    let dropout: Bool
}

struct TouchReticleState {
    var normalizedPoint: CGPoint = CGPoint(x: 0.5, y: 0.5)
    var visible: Bool = false
    var locked: Bool = false
    var adjustingExposure: Bool = false
    var adjustingFocus: Bool = false
    var adjustingZoom: Bool = false
    var toastText: String? = nil
    var toastIcon: String = "viewfinder"
}

struct CameraTelemetry {
    var timecode: String = "01:00:00:00"
    var bitrateMbps: Double = 35.0
    var latencyMs: Double = 8.4
    var jitterMs: Double = 0.35
    var lossPct: Double = 0.0
    var bufferPct: Int = 98
    var actualFps: Double = 60.0
    var codecBackend: String = "VideoToolbox HW"
    var micPeakDbfs: Float = -24.0
    var ifbPeakDbfs: Float = -32.0
    var batteryPct: Int = 92
    var tempC: Int = 34
    var history: [TelemetrySample] = (0..<36).map { i in
        TelemetrySample(
            bitrateMbps: 34.5 + sin(Double(i) * 0.22) * 0.8,
            latencyMs: 8.2 + cos(Double(i) * 0.18) * 0.4,
            dropout: false
        )
    }
    var histogram: [Float] = [
        0.12, 0.22, 0.35, 0.48, 0.62, 0.74, 0.85, 0.78,
        0.65, 0.58, 0.68, 0.82, 0.90, 0.72, 0.50, 0.34,
        0.26, 0.31, 0.44, 0.38, 0.22, 0.14, 0.08, 0.05
    ]
}
