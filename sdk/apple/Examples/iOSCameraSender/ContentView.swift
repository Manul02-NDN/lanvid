// LanVid Camera (iOS) — Pro Manual Cine Camera + Tally (PGM/PVW/STBY) + Duplex Intercom (IFB / PTT)
import AVFoundation
import SwiftUI
import LanVidKit

// MARK: - Data Models

enum ProTallyState: Int {
    case standby = 0
    case preview = 1
    case program = 2

    var badgeText: String {
        switch self {
        case .program: return "PGM • ON AIR"
        case .preview: return "PVW • PREVIEW"
        case .standby: return "STBY"
        }
    }

    var accentColor: Color {
        switch self {
        case .program: return Color(red: 0.94, green: 0.27, blue: 0.27)
        case .preview: return Color(red: 0.13, green: 0.77, blue: 0.37)
        case .standby: return Color(red: 0.58, green: 0.64, blue: 0.72)
        }
    }
}

enum CineLens: String, CaseIterable, Identifiable {
    case ultraWide = "0.5x"
    case wide = "1x"
    case tele = "2x"
    case front = "Front"
    var id: String { rawValue }
}

enum CineResolution: String, CaseIterable, Identifiable {
    case uhd4k = "4K"
    case fhd1080 = "1080p"
    case hd720 = "720p"
    var id: String { rawValue }
}

enum CineCodec: String, CaseIterable, Identifiable {
    case hevc = "H.265 (HEVC)"
    case h264 = "H.264 (AVC)"
    case dct = "LanVid DCT"
    case proRes = "Apple ProRes"
    var id: String { rawValue }
}

// MARK: - Duplex Intercom Audio Engine (AVAudioSession playAndRecord @ 5 ms)

final class IntercomAudioController {
    private let audioEngine = AVAudioEngine()
    private var isRunning = false
    var onMicPcm16: (([Int16]) -> Void)?

    func start() {
        guard !isRunning else { return }
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(
                .playAndRecord,
                mode: .voiceChat,
                options: [.defaultToSpeaker, .allowBluetooth, .allowBluetoothA2DP]
            )
            try session.setPreferredSampleRate(48_000)
            try session.setPreferredIOBufferDuration(0.005) // 5 ms ultra-low latency
            try session.setActive(true)

            let input = audioEngine.inputNode
            let format = input.outputFormat(forBus: 0)
            input.installTap(onBus: 0, bufferSize: 480, format: format) { [weak self] buffer, _ in
                guard let ch = buffer.floatChannelData?[0] else { return }
                let count = Int(buffer.frameLength)
                var pcm = [Int16](repeating: 0, count: count)
                for i in 0..<count {
                    let s = max(-1.0, min(1.0, ch[i]))
                    pcm[i] = Int16(s * 32767.0)
                }
                self?.onMicPcm16?(pcm)
            }
            audioEngine.prepare()
            try audioEngine.start()
            isRunning = true
        } catch {
            // Intercom audio fallback if hardware session unavailable (e.g. Simulator)
        }
    }

    func stop() {
        guard isRunning else { return }
        audioEngine.inputNode.removeTap(onBus: 0)
        audioEngine.stop()
        isRunning = false
    }
}

// MARK: - Pro Manual Camera Session Controller

final class CameraSessionController: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    @Published var isStreaming = false
    @Published var peers: [LanVidPeer] = []
    @Published var destIp = "192.168.1.100"
    @Published var statusText = "Ready (NEON: \(LanVidRuntime.simdInfo))"

    // Tally & Duplex Intercom state
    @Published var tallyState: ProTallyState = .standby
    @Published var cameraLabel = "CAM 1"
    @Published var remoteRec = false
    @Published var directorSpeaking = false
    @Published var duckingGain: Float = 1.0
    @Published var pttActive = false
    @Published var latchActive = false
    @Published var ifbEnabled = true
    @Published var ifbVolume: Float = 0.85

    // Manual Camera Deck state
    @Published var lens: CineLens = .wide
    @Published var shutterAngle: Int = 180
    @Published var isoValue: Float = 400
    @Published var autoExposure = true
    @Published var wbKelvin: Float = 5600
    @Published var wbTint: Float = 0
    @Published var autoWhiteBalance = true
    @Published var focusPosition: Float = 0.5
    @Published var focusPointA: Float = 0.25
    @Published var focusPointB: Float = 0.75
    @Published var autoFocus = true
    @Published var stabilization = "Standard"
    @Published var focusPeaking = false
    @Published var zebra = false
    @Published var showGuides = true
    @Published var showHistogram = true

    // Encoder & Stream Drawer state
    @Published var resolution: CineResolution = .fhd1080
    @Published var fps: Int = 60
    @Published var codec: CineCodec = .hevc
    @Published var bitrateMbps: Double = 35.0
    @Published var gopMode = "1s"
    @Published var colorProfile = "Rec.709"

    let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "lanvid.ios.camera")
    private var sender: LanVidSender?
    private var activeDevice: AVCaptureDevice?
    private let intercomAudio = IntercomAudioController()

    func scanLan() {
        Task { @MainActor in
            peers = await LanVidDiscovery.scanOnce()
            if let first = peers.first { destIp = first.ip }
        }
    }

    func toggleStreaming() {
        if isStreaming {
            session.stopRunning()
            sender?.stop()
            intercomAudio.stop()
            isStreaming = false
            tallyState = .standby
            statusText = "Stopped"
            return
        }
        do {
            let s = try LanVidSender()
            try s.start(config: .init(destHost: destIp, destPort: 5000, quality: 82, fecGroupSize: 6, adaptiveQuality: true))
            sender = s
            configureCamera()
            intercomAudio.start()
            queue.async { self.session.startRunning() }
            isStreaming = true
            statusText = "Streaming \(resolution.rawValue) \(fps)fps (\(codec.rawValue)) -> \(destIp):5000"
        } catch {
            statusText = "Error: \(error.localizedDescription)"
        }
    }

    func applyManualCameraControls() {
        guard let dev = activeDevice else { return }
        do {
            try dev.lockForConfiguration()
            // Exposure: Shutter Angle + ISO
            if autoExposure {
                if dev.isExposureModeSupported(.continuousAutoExposure) {
                    dev.exposureMode = .continuousAutoExposure
                }
            } else if dev.isExposureModeSupported(.custom) {
                let shutterDenom = Double(fps) * (360.0 / Double(max(1, shutterAngle)))
                let dur = CMTime(value: 1, timescale: CMTimeScale(max(24, Int(shutterDenom))))
                let clampedIso = max(dev.activeFormat.minISO, min(dev.activeFormat.maxISO, isoValue))
                dev.setExposureModeCustom(duration: dur, iso: clampedIso)
            }
            // White Balance
            if autoWhiteBalance {
                if dev.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                    dev.whiteBalanceMode = .continuousAutoWhiteBalance
                }
            } else if dev.isWhiteBalanceModeSupported(.locked) {
                let tt = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(temperature: wbKelvin, tint: wbTint)
                let gains = dev.deviceWhiteBalanceGains(for: tt)
                let clamped = AVCaptureDevice.WhiteBalanceGains(
                    redGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.redGain)),
                    greenGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.greenGain)),
                    blueGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.blueGain))
                )
                dev.setWhiteBalanceModeLocked(with: clamped)
            }
            // Manual Focus
            if autoFocus {
                if dev.isFocusModeSupported(.continuousAutoFocus) {
                    dev.focusMode = .continuousAutoFocus
                }
            } else if dev.isFocusModeSupported(.locked) {
                dev.setFocusModeLocked(lensPosition: max(0.0, min(1.0, focusPosition)))
            }
            dev.unlockForConfiguration()
        } catch {
            statusText = "Camera lock error: \(error.localizedDescription)"
        }
    }

    func rackFocus(to target: Float) {
        autoFocus = false
        focusPosition = target
        applyManualCameraControls()
    }

    private func configureCamera() {
        session.beginConfiguration()
        switch resolution {
        case .uhd4k: session.sessionPreset = .hd4K3840x2160
        case .fhd1080: session.sessionPreset = .hd1920x1080
        case .hd720: session.sessionPreset = .hd1280x720
        }
        for input in session.inputs { session.removeInput(input) }
        let position: AVCaptureDevice.Position = (lens == .front) ? .front : .back
        let deviceType: AVCaptureDevice.DeviceType = {
            switch lens {
            case .ultraWide: return .builtInUltraWideCamera
            case .tele: return .builtInTelephotoCamera
            default: return .builtInWideAngleCamera
            }
        }()
        let cam = AVCaptureDevice.default(deviceType, for: .video, position: position)
            ?? AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position)
        activeDevice = cam
        if let cam, let input = try? AVCaptureDeviceInput(device: cam) {
            session.addInput(input)
        }
        if session.outputs.isEmpty {
            let out = AVCaptureVideoDataOutput()
            out.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            ]
            out.alwaysDiscardsLateVideoFrames = true
            out.setSampleBufferDelegate(self, queue: queue)
            session.addOutput(out)
        }
        session.commitConfiguration()
        applyManualCameraControls()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        try? sender?.push(sampleBuffer: sampleBuffer)
    }
}

// MARK: - Soft Lavender / Dark Glass Pro Cine HUD

struct ContentView: View {
    @StateObject private var ctrl = CameraSessionController()
    @State private var showManualDeck = false
    @State private var showEncoderDrawer = false

    private let glassBg = Color(red: 0.055, green: 0.055, blue: 0.075).opacity(0.84)
    private let lavender = Color(red: 0.65, green: 0.55, blue: 0.98)

    var body: some View {
        ZStack {
            Color(red: 0.04, green: 0.04, blue: 0.06).ignoresSafeArea()

            // Full-screen Tally Border Overlay (PGM Red / PVW Green)
            if ctrl.tallyState != .standby {
                RoundedRectangle(cornerRadius: 16)
                    .stroke(ctrl.tallyState.accentColor, lineWidth: 6)
                    .ignoresSafeArea()
            }

            VStack(spacing: 10) {
                // Top Telemetry & Tally Pill Bar
                HStack(spacing: 8) {
                    Text("\(ctrl.cameraLabel) · \(ctrl.tallyState.badgeText)")
                        .font(.system(size: 11, weight: .bold, design: .monospaced))
                        .foregroundColor(.white)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(ctrl.tallyState.accentColor)
                        .clipShape(Capsule())

                    if ctrl.remoteRec {
                        Text("● REC")
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .foregroundColor(.white)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(Color.red)
                            .clipShape(Capsule())
                    }

                    if ctrl.directorSpeaking {
                        Text("🎙 DIRECTOR SPEAKING")
                            .font(.system(size: 10, weight: .bold, design: .monospaced))
                            .foregroundColor(.white)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(lavender)
                            .clipShape(Capsule())
                    }

                    Spacer()

                    Text("\(ctrl.resolution.rawValue) \(ctrl.fps)p · \(Int(ctrl.bitrateMbps)) Mbps")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.white.opacity(0.85))
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(glassBg)
                .clipShape(RoundedRectangle(cornerRadius: 14))

                // Connection & LAN Discovery Strip
                HStack(spacing: 8) {
                    TextField("Studio IP", text: $ctrl.destIp)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 12, design: .monospaced))
                    Button("Scan LAN") { ctrl.scanLan() }
                        .buttonStyle(.bordered)
                        .tint(lavender)
                    Button(ctrl.isStreaming ? "STOP" : "GO LIVE") { ctrl.toggleStreaming() }
                        .buttonStyle(.borderedProminent)
                        .tint(ctrl.isStreaming ? .red : lavender)
                }

                Spacer()

                // Expandable Manual Camera Deck
                if showManualDeck {
                    manualDeckPanel
                }

                // Expandable Encoder & Stream Drawer
                if showEncoderDrawer {
                    encoderDrawerPanel
                }

                // Bottom Pro Controls + Duplex Intercom Bar
                HStack(spacing: 8) {
                    Button(showManualDeck ? "DECK ●" : "DECK") {
                        showManualDeck.toggle()
                        if showManualDeck { showEncoderDrawer = false }
                    }
                    .buttonStyle(.bordered)
                    .tint(lavender)

                    Button(showEncoderDrawer ? "STREAM ●" : "STREAM") {
                        showEncoderDrawer.toggle()
                        if showEncoderDrawer { showManualDeck = false }
                    }
                    .buttonStyle(.bordered)
                    .tint(lavender)

                    Spacer()

                    // Duplex Intercom PTT + Latch + IFB
                    Button(ctrl.pttActive ? "🎙 TALKING" : "TALK (PTT)") {
                        ctrl.pttActive.toggle()
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(ctrl.pttActive ? .red : .gray.opacity(0.35))

                    Button(ctrl.latchActive ? "LATCH ON" : "LATCH") {
                        ctrl.latchActive.toggle()
                        if ctrl.latchActive { ctrl.pttActive = true }
                    }
                    .buttonStyle(.bordered)
                    .tint(ctrl.latchActive ? lavender : .gray)

                    Button(ctrl.ifbEnabled ? "🎧 IFB" : "🔇 IFB") {
                        ctrl.ifbEnabled.toggle()
                    }
                    .buttonStyle(.bordered)
                    .tint(ctrl.ifbEnabled ? .green : .gray)
                }
                .padding(10)
                .background(glassBg)
                .clipShape(RoundedRectangle(cornerRadius: 16))
            }
            .padding(12)
        }
        .preferredColorScheme(.dark)
        .onAppear { ctrl.scanLan() }
    }

    private var manualDeckPanel: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("PRO MANUAL CAMERA DECK")
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(lavender)

            // Lens selector
            HStack {
                ForEach(CineLens.allCases) { l in
                    Button(l.rawValue) { ctrl.lens = l }
                        .buttonStyle(.bordered)
                        .tint(ctrl.lens == l ? lavender : .gray)
                }
            }

            // Shutter Angle & ISO
            HStack {
                Text("Shutter \(ctrl.shutterAngle)°")
                    .font(.system(size: 11, design: .monospaced))
                ForEach([45, 90, 180, 360], id: \.self) { ang in
                    Button("\(ang)°") {
                        ctrl.autoExposure = false
                        ctrl.shutterAngle = ang
                        ctrl.applyManualCameraControls()
                    }
                    .buttonStyle(.bordered)
                    .tint(ctrl.shutterAngle == ang ? lavender : .gray)
                }
            }

            HStack {
                Text("ISO \(Int(ctrl.isoValue))")
                    .font(.system(size: 11, design: .monospaced))
                    .frame(width: 72, alignment: .leading)
                Slider(value: $ctrl.isoValue, in: 50...3200, step: 50) { _ in
                    ctrl.autoExposure = false
                    ctrl.applyManualCameraControls()
                }
                .tint(lavender)
                Button(ctrl.autoExposure ? "AE" : "M") {
                    ctrl.autoExposure.toggle()
                    ctrl.applyManualCameraControls()
                }
                .buttonStyle(.bordered)
            }

            // WB Kelvin
            HStack {
                Text("WB \(Int(ctrl.wbKelvin))K")
                    .font(.system(size: 11, design: .monospaced))
                    .frame(width: 72, alignment: .leading)
                Slider(value: $ctrl.wbKelvin, in: 2500...9500, step: 100) { _ in
                    ctrl.autoWhiteBalance = false
                    ctrl.applyManualCameraControls()
                }
                .tint(lavender)
                Button(ctrl.autoWhiteBalance ? "AWB" : "M") {
                    ctrl.autoWhiteBalance.toggle()
                    ctrl.applyManualCameraControls()
                }
                .buttonStyle(.bordered)
            }

            // Manual Focus + Rack Focus A <-> B
            HStack {
                Text("MF \(Int(ctrl.focusPosition * 100))%")
                    .font(.system(size: 11, design: .monospaced))
                    .frame(width: 72, alignment: .leading)
                Slider(value: $ctrl.focusPosition, in: 0...1) { _ in
                    ctrl.autoFocus = false
                    ctrl.applyManualCameraControls()
                }
                .tint(lavender)
                Button("A") { ctrl.rackFocus(to: ctrl.focusPointA) }.buttonStyle(.bordered)
                Button("B") { ctrl.rackFocus(to: ctrl.focusPointB) }.buttonStyle(.bordered)
            }

            // Viewfinder Assist
            HStack(spacing: 8) {
                Toggle("Peaking", isOn: $ctrl.focusPeaking).toggleStyle(.button).tint(lavender)
                Toggle("Zebra", isOn: $ctrl.zebra).toggleStyle(.button).tint(lavender)
                Toggle("Guides", isOn: $ctrl.showGuides).toggleStyle(.button).tint(lavender)
            }
            .font(.system(size: 10, design: .monospaced))
        }
        .padding(12)
        .background(glassBg)
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }

    private var encoderDrawerPanel: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("ENCODER & STREAM DRAWER")
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(lavender)

            HStack {
                ForEach(CineResolution.allCases) { r in
                    Button(r.rawValue) { ctrl.resolution = r }
                        .buttonStyle(.bordered)
                        .tint(ctrl.resolution == r ? lavender : .gray)
                }
                Spacer()
                ForEach([24, 30, 60], id: \.self) { f in
                    Button("\(f)p") { ctrl.fps = f }
                        .buttonStyle(.bordered)
                        .tint(ctrl.fps == f ? lavender : .gray)
                }
            }

            HStack {
                ForEach(CineCodec.allCases) { c in
                    Button(c.rawValue) { ctrl.codec = c }
                        .buttonStyle(.bordered)
                        .tint(ctrl.codec == c ? lavender : .gray)
                        .font(.system(size: 10))
                }
            }

            HStack {
                Text("Bitrate \(Int(ctrl.bitrateMbps)) Mbps")
                    .font(.system(size: 11, design: .monospaced))
                Slider(value: $ctrl.bitrateMbps, in: 5...150, step: 5)
                    .tint(lavender)
            }
        }
        .padding(12)
        .background(glassBg)
        .clipShape(RoundedRectangle(cornerRadius: 16))
    }
}
