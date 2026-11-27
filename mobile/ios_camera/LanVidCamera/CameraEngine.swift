@preconcurrency import AVFoundation
import Foundation
import LanVidCore
import SwiftUI
import UIKit

/// Duplex Intercom audio controller (AVAudioSession playAndRecord @ 48 kHz, 5 ms buffer, background-safe).
/// Resamples microphone input to 48 kHz mono Float32 for `lv_intercom_push_mic_pcm` and plays received
/// director IFB voice via an `AVAudioSourceNode` pulling from `lv_intercom_pull_rx_pcm`.
final class IntercomAudioController {
    private let audioEngine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?
    private var isRunning = false
    private var shouldRun = false
    private var observers: [NSObjectProtocol] = []

    var onMicFloat: ((UnsafePointer<Float>, Int) -> Void)?
    var onMicLevel: ((Float) -> Void)?
    var onPullIfbFloat: ((UnsafeMutablePointer<Float>, Int) -> Bool)?
    var onIfbLevel: ((Float) -> Void)?

    init() {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            self?.handleInterruption(note)
        })
        observers.append(nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self, self.shouldRun else { return }
            self.restart()
        })
    }

    deinit {
        for obs in observers { NotificationCenter.default.removeObserver(obs) }
        stop()
    }

    func start() {
        shouldRun = true
        guard !isRunning else { return }
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(
                .playAndRecord,
                mode: .voiceChat,
                options: [.defaultToSpeaker, .allowBluetooth, .allowBluetoothA2DP, .mixWithOthers]
            )
            try session.setPreferredSampleRate(48_000)
            try session.setPreferredIOBufferDuration(0.005)
            try session.setActive(true)

            guard let targetFmt = AVAudioFormat(
                commonFormat: .pcmFormatFloat32,
                sampleRate: 48_000,
                channels: 1,
                interleaved: false
            ) else { return }

            let node = AVAudioSourceNode(format: targetFmt) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
                let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
                let count = Int(frameCount)
                guard let dst = abl.first?.mData?.assumingMemoryBound(to: Float.self), count > 0 else {
                    return noErr
                }
                let gotAudio = self?.onPullIfbFloat?(dst, count) ?? false
                if !gotAudio {
                    memset(dst, 0, count * MemoryLayout<Float>.size)
                    self?.onIfbLevel?(-60.0)
                    return noErr
                }
                var peak: Float = 0.00001
                for i in 0..<count {
                    let a = abs(dst[i])
                    if a > peak { peak = a }
                }
                self?.onIfbLevel?(max(-60.0, 20.0 * log10(peak)))
                return noErr
            }
            audioEngine.attach(node)
            audioEngine.connect(node, to: audioEngine.mainMixerNode, format: targetFmt)
            sourceNode = node

            let input = audioEngine.inputNode
            let hwFormat = input.outputFormat(forBus: 0)
            if hwFormat.channelCount > 0 && hwFormat.sampleRate > 0 {
                let needsConvert = (hwFormat.sampleRate != 48_000.0 || hwFormat.channelCount != 1 || hwFormat.commonFormat != .pcmFormatFloat32)
                let converter: AVAudioConverter? = needsConvert ? AVAudioConverter(from: hwFormat, to: targetFmt) : nil
                let convBuf = needsConvert ? AVAudioPCMBuffer(pcmFormat: targetFmt, frameCapacity: 2048) : nil

                input.installTap(onBus: 0, bufferSize: 480, format: hwFormat) { [weak self] buffer, _ in
                    guard let self else { return }
                    let outPtr: UnsafePointer<Float>
                    let outCount: Int

                    if let converter, let convBuf {
                        let ratio = 48_000.0 / max(1.0, hwFormat.sampleRate)
                        let estFrames = AVAudioFrameCount(min(Double(convBuf.frameCapacity), ceil(Double(buffer.frameLength) * ratio) + 16))
                        convBuf.frameLength = estFrames
                        var provided = false
                        var err: NSError?
                        converter.convert(to: convBuf, error: &err) { _, outStatus in
                            if provided {
                                outStatus.pointee = .noDataNow
                                return nil
                            }
                            provided = true
                            outStatus.pointee = .haveData
                            return buffer
                        }
                        guard err == nil, convBuf.frameLength > 0, let ch = convBuf.floatChannelData?[0] else { return }
                        outPtr = UnsafePointer(ch)
                        outCount = Int(convBuf.frameLength)
                    } else {
                        guard let ch = buffer.floatChannelData?[0], buffer.frameLength > 0 else { return }
                        outPtr = UnsafePointer(ch)
                        outCount = Int(buffer.frameLength)
                    }

                    var peak: Float = 0.00001
                    for i in 0..<outCount {
                        let a = abs(outPtr[i])
                        if a > peak { peak = a }
                    }
                    self.onMicLevel?(max(-60.0, 20.0 * log10(peak)))
                    self.onMicFloat?(outPtr, outCount)
                }
            }

            audioEngine.prepare()
            try audioEngine.start()
            isRunning = true
        } catch {
            cleanupEngine()
        }
    }

    func stop() {
        shouldRun = false
        cleanupEngine()
    }

    private func cleanupEngine() {
        if isRunning {
            audioEngine.inputNode.removeTap(onBus: 0)
            audioEngine.stop()
            isRunning = false
        }
        if let node = sourceNode {
            audioEngine.detach(node)
            sourceNode = nil
        }
    }

    private func restart() {
        cleanupEngine()
        if shouldRun { start() }
    }

    private func handleInterruption(_ note: Notification) {
        guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
        if type == .began {
            cleanupEngine()
        } else if type == .ended && shouldRun {
            start()
        }
    }
}

/// Complete Pro Cine Camera, Hardware VideoToolbox / LanVidCore Sender, Multi-Level Stabilization,
/// Intuitive Touch Controls (Zoom, Exposure, Focus, AE/AF Lock), Tally + Duplex Intercom,
/// AVCaptureMultiCamSession Dual-Cam (PIP / 50-50 Split), iPhone Duo Sync, and Demo Cine Studio engine.
@MainActor
final class CameraEngine: NSObject, ObservableObject {
    // MARK: - Studio Appearance
    @Published var themeMode: StudioThemeMode {
        didSet { UserDefaults.standard.set(themeMode.rawValue, forKey: "cam_theme_mode") }
    }
    @Published var cleanHUD: Bool = false

    // MARK: - Connection & Lifecycle
    @Published var destIp: String {
        didSet { UserDefaults.standard.set(destIp, forKey: "cam_dest_ip") }
    }
    @Published var videoPort: Int {
        didSet { UserDefaults.standard.set(videoPort, forKey: "cam_video_port") }
    }
    @Published var tallyPort: Int {
        didSet { UserDefaults.standard.set(tallyPort, forKey: "cam_tally_port") }
    }
    @Published var cameraLabel: String {
        didSet { UserDefaults.standard.set(cameraLabel, forKey: "cam_label") }
    }
    @Published var assignedCamNumber: Int {
        didSet { UserDefaults.standard.set(assignedCamNumber, forKey: "cam_assigned_number") }
    }
    @Published var assignedCamTag: String {
        didSet { UserDefaults.standard.set(assignedCamTag, forKey: "cam_assigned_tag") }
    }
    @Published var configSynced = false
    @Published var syncedDelayMs: Int = 33
    @Published var syncedFixedDelay = true
    @Published var isStreaming = false
    @Published var isPreviewActive = false
    @Published var isDemoMode = false
    @Published var statusText = ""

    var displayBadgeTag: String {
        if !assignedCamTag.isEmpty { return assignedCamTag }
        if assignedCamNumber > 0 { return "CAM \(assignedCamNumber)" }
        return cameraLabel.isEmpty ? "CAM A" : cameraLabel
    }

    // MARK: - Tally & Duplex Intercom
    @Published var tallyState: ProTallyState = .standby
    @Published var remoteRec = false
    @Published var directorSpeaking = false
    @Published var duckingGain: Float = 1.0
    @Published var pttActive = false { didSet { syncIntercomState() } }
    @Published var latchActive = false {
        didSet {
            pttActive = latchActive
            syncIntercomState()
        }
    }
    @Published var ifbEnabled = true { didSet { syncIntercomState() } }
    @Published var ifbVolume: Float = 0.85 { didSet { syncIntercomState() } }
    @Published var micGain: Float = 1.0

    // MARK: - Manual Pro Cine Camera Controls & Touch Gestures
    @Published var lens: CineLens = .wide
    @Published var zoomFactor: CGFloat = 1.0
    @Published var minDisplayZoom: CGFloat = 0.5
    @Published var maxDisplayZoom: CGFloat = 10.0
    @Published var shutterAngle: Int = 180
    @Published var isoValue: Float = 400
    @Published var autoExposure = true
    @Published var exposureBias: Float = 0.0
    @Published var wbKelvin: Float = 5600
    @Published var wbTint: Float = 0
    @Published var autoWhiteBalance = true
    @Published var focusPosition: Float = 0.48
    @Published var focusPointA: Float = 0.25
    @Published var focusPointB: Float = 0.78
    @Published var autoFocus = true
    @Published var aeAfLocked = false
    @Published var focusPeaking = false
    @Published var zebra = false
    @Published var showGuides = true
    @Published var showHistogram = true
    @Published var touchHUD = TouchReticleState()

    // MARK: - Multi-Level Image Stabilization
    @Published var stabilizationLevel: StabilizationLevel {
        didSet { UserDefaults.standard.set(stabilizationLevel.rawValue, forKey: "cam_stabilization_level") }
    }
    @Published var activeHardwareStabilization: StabilizationLevel = .standard
    @Published var supportedStabilizationLevels: [StabilizationLevel] = StabilizationLevel.allCases

    // MARK: - Secondary Camera Exposure (Dual-Cam PIP / Split)
    @Published var secondaryAutoExposure = true
    @Published var secondaryExposureBias: Float = 0.0

    // MARK: - Encoder & Stream Profile
    @Published var resolution: CineResolution = .fhd1080 {
        didSet {
            guard oldValue != resolution else { return }
            if !isApplyingRemoteSync {
                sendLocalConfigSyncToStudio()
                if isStreaming && !isDemoMode {
                    reconfigureVideoSenderOnly()
                } else if isPreviewActive && !isDemoMode && duoMode != .duoCompanion {
                    configureAndStartCapture()
                }
            }
        }
    }
    @Published var fps: Int = 60 {
        didSet {
            guard oldValue != fps else { return }
            if !isApplyingRemoteSync {
                sendLocalConfigSyncToStudio()
                if isStreaming && !isDemoMode {
                    reconfigureVideoSenderOnly()
                } else if isPreviewActive && !isDemoMode && duoMode != .duoCompanion {
                    configureAndStartCapture()
                }
            }
        }
    }
    @Published var codec: CineCodec = .hevc {
        didSet {
            guard oldValue != codec else { return }
            if !isApplyingRemoteSync {
                sendLocalConfigSyncToStudio()
                if isStreaming && !isDemoMode { reconfigureVideoSenderOnly() }
            }
        }
    }
    @Published var bitrateMbps: Double = 35.0 {
        didSet {
            if isStreaming && !isDemoMode {
                senderLock.lock()
                let s = senderHandle
                if let s {
                    lv_sender_set_bitrate(s, UInt32(max(2000, Int(bitrateMbps * 1000.0))))
                }
                senderLock.unlock()
            }
            if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() }
        }
    }
    @Published var adaptiveQuality: Bool = true
    @Published var fecEnabled: Bool = true
    @Published var colorProfile = "Rec.709"
    private var isApplyingRemoteSync = false
    private var localConfigSyncRev: UInt64 = 0

    // MARK: - iPhone Duo Mode
    @Published var duoMode: DuoMode = .singleHUD
    @Published var duoRole: DuoRole = .camA
    @Published var selectedDuoPeerHost: String?
    @Published var multiCamSupported: Bool = AVCaptureMultiCamSession.isMultiCamSupported

    // MARK: - Telemetry
    @Published var telemetry = CameraTelemetry()

    // MARK: - Discovery & Duo Sync
    let browser = DuoSyncBrowser()

    // MARK: - AVFoundation Sessions & Virtual Multi-Lens Zoom Mapping
    nonisolated let singleSession = AVCaptureSession()
    nonisolated let multiCamSession: AVCaptureMultiCamSession? = AVCaptureMultiCamSession.isMultiCamSupported ? AVCaptureMultiCamSession() : nil
    private let captureQueue = DispatchQueue(label: "com.manul02.lanvid.camera.capture", qos: .userInteractive)
    private var primaryDevice: AVCaptureDevice?
    private var secondaryDevice: AVCaptureDevice?
    private var isUsingVirtualRearDevice = false
    private var wideBaseZoomFactor: CGFloat = 1.0
    private var sessionObservers: [NSObjectProtocol] = []

    // Haptics & Reticle auto-hide tasks
    private let lightImpact = UIImpactFeedbackGenerator(style: .light)
    private let mediumImpact = UIImpactFeedbackGenerator(style: .medium)
    private let selectionFeedback = UISelectionFeedbackGenerator()
    private var reticleHideTask: Task<Void, Never>?
    private var toastHideTask: Task<Void, Never>?
    private var lastEvHapticZeroCross: Bool = true
    private var lastZoomDetent: Int = 10

    // MARK: - LanVidCore C ABI Handles (all cross-thread access guarded by locks)
    private let senderLock = NSLock()
    private nonisolated(unsafe) var senderHandle: OpaquePointer?

    private let tallyLock = NSLock()
    private nonisolated(unsafe) var tallyHandle: OpaquePointer?
    private nonisolated(unsafe) var isMicTalkOpen: Bool = false
    private nonisolated(unsafe) var isIfbListenOpen: Bool = true
    private let tallyPollQueue = DispatchQueue(label: "com.manul02.lanvid.camera.tally", qos: .userInteractive)
    private var tallyPollTimer: DispatchSourceTimer?

    private let intercomAudio = IntercomAudioController()
    private var tickTimer: Timer?
    private var streamStartWallTime: CFTimeInterval = CACurrentMediaTime()
    private var demoPhaseCounter: Int = 0
    private let cameraId: UInt32

    override init() {
        let ud = UserDefaults.standard
        self.themeMode = StudioThemeMode(rawValue: ud.string(forKey: "cam_theme_mode") ?? "") ?? .dark
        self.destIp = ud.string(forKey: "cam_dest_ip") ?? "192.168.1.24"
        let vp = ud.integer(forKey: "cam_video_port")
        self.videoPort = vp > 0 ? vp : 5006
        let tp = ud.integer(forKey: "cam_tally_port")
        self.tallyPort = tp > 0 ? tp : 5022
        self.cameraLabel = ud.string(forKey: "cam_label") ?? "CAM A"
        self.assignedCamNumber = ud.integer(forKey: "cam_assigned_number")
        self.assignedCamTag = ud.string(forKey: "cam_assigned_tag") ?? ""
        let savedStab = StabilizationLevel(rawValue: ud.string(forKey: "cam_stabilization_level") ?? "") ?? .standard
        self.stabilizationLevel = savedStab
        self.activeHardwareStabilization = savedStab

        let storedId = ud.integer(forKey: "cam_unique_id")
        if storedId > 0 {
            self.cameraId = UInt32(storedId)
        } else {
            let newId = UInt32.random(in: 1000...999_999)
            ud.set(Int(newId), forKey: "cam_unique_id")
            self.cameraId = newId
        }
        super.init()

        lv_init()
        UIDevice.current.isBatteryMonitoringEnabled = true
        updateThermalTemperature()
        let simd = lv_get_cpu_simd_info().map { String(cString: $0) } ?? "NEON"
        self.statusText = tr("Готово • ", "Ready • ") + simd

        let nc = NotificationCenter.default
        sessionObservers.append(nc.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.updateThermalTemperature() }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.isStreaming, !self.isDemoMode else { return }
                self.statusText = tr("Камеру призупинено системою", "Camera session interrupted")
            }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self, self.isStreaming, !self.isDemoMode else { return }
                self.statusText = "\(self.resolution.rawValue) \(self.fps)p \(self.codec.shortName) → \(self.destIp):\(self.videoPort)"
            }
        })
        sessionObservers.append(nc.addObserver(forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in self?.syncCaptureConnectionOrientation() }
        })

        browser.onRemoteDuoCommand = { [weak self] tally, focus, iso, shutter, wb, tc in
            guard let self else { return }
            if let t = tally { self.tallyState = t }
            if let f = focus {
                self.autoFocus = false
                self.focusPosition = f
                self.applyManualCameraControls()
            }
            if let i = iso {
                self.autoExposure = false
                self.isoValue = i
                self.applyManualCameraControls()
            }
            if let sh = shutter {
                self.autoExposure = false
                self.shutterAngle = sh
                self.applyManualCameraControls()
            }
            if let w = wb {
                self.autoWhiteBalance = false
                self.wbKelvin = w
                self.applyManualCameraControls()
            }
            if let tc, !tc.isEmpty {
                self.telemetry.timecode = tc
            }
        }

        intercomAudio.onMicLevel = { [weak self] dbfs in
            Task { @MainActor in self?.telemetry.micPeakDbfs = dbfs }
        }
        intercomAudio.onIfbLevel = { [weak self] dbfs in
            Task { @MainActor in self?.telemetry.ifbPeakDbfs = dbfs }
        }
        intercomAudio.onMicFloat = { [weak self] ptr, count in
            guard let self else { return }
            self.tallyLock.lock()
            defer { self.tallyLock.unlock() }
            if let th = self.tallyHandle, self.isMicTalkOpen {
                lv_intercom_push_mic_pcm(th, ptr, Int32(count))
            }
        }
        intercomAudio.onPullIfbFloat = { [weak self] dst, count in
            guard let self else { return false }
            guard self.tallyLock.try() else { return false }
            defer { self.tallyLock.unlock() }
            guard let th = self.tallyHandle, self.isIfbListenOpen else { return false }
            return lv_intercom_pull_rx_pcm(th, dst, Int32(count)) > 0
        }
    }

    deinit {
        for obs in sessionObservers { NotificationCenter.default.removeObserver(obs) }
        reticleHideTask?.cancel()
        toastHideTask?.cancel()
        tickTimer?.invalidate()
        tallyPollTimer?.cancel()
        tallyPollTimer = nil
        intercomAudio.stop()
        if singleSession.isRunning { singleSession.stopRunning() }
        if let mc = multiCamSession, mc.isRunning { mc.stopRunning() }
        senderLock.lock()
        let s = senderHandle
        senderHandle = nil
        senderLock.unlock()
        if let s {
            lv_sender_stop(s)
            lv_sender_destroy(s)
        }
        tallyLock.lock()
        let t = tallyHandle
        tallyHandle = nil
        tallyLock.unlock()
        if let t {
            lv_tally_intercom_destroy(t)
        }
    }

    // MARK: - Theme Toggle

    func toggleTheme() {
        withAnimation(.easeInOut(duration: 0.16)) {
            themeMode = (themeMode == .dark) ? .light : .dark
        }
    }

    // MARK: - Local Camera Preview (immediate live framing before GO LIVE)

    func startLocalPreviewIfAuthorized() {
        #if targetEnvironment(simulator)
        return
        #else
        guard !isDemoMode, duoMode != .duoCompanion else { return }
        let status = AVCaptureDevice.authorizationStatus(for: .video)
        if status == .authorized {
            configureAndStartCapture()
            isPreviewActive = true
        } else if status == .notDetermined {
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                Task { @MainActor in
                    guard let self, granted, !self.isDemoMode else { return }
                    self.configureAndStartCapture()
                    self.isPreviewActive = true
                }
            }
        }
        #endif
    }

    // MARK: - Discovery & Demo Cine Studio

    func startDiscovery() {
        browser.start(localRole: duoRole, localLabel: cameraLabel, demoMode: isDemoMode)
    }

    func selectStudio(_ studio: DiscoveredStudio) {
        destIp = studio.host
        videoPort = Int(studio.videoPort)
        tallyPort = Int(studio.tallyPort)
        statusText = tr("Обрано студію: ", "Selected studio: ") + studio.name
        showTouchToast(studio.name, icon: "desktopcomputer")
    }

    /// Launches a rich interactive Demo Cine Studio (no PC or hardware camera required) for exploration & screenshots.
    func startDemoCineStudio(tally: ProTallyState = .preview, mode: DuoMode = .singleHUD) {
        stopStreaming()
        isDemoMode = true
        isStreaming = true
        isPreviewActive = true
        UIApplication.shared.isIdleTimerDisabled = true
        streamStartWallTime = CACurrentMediaTime()
        duoMode = mode
        tallyState = tally
        remoteRec = (tally == .program)
        directorSpeaking = (tally == .program)
        isApplyingRemoteSync = true
        resolution = .uhd4k
        fps = 60
        codec = .hevc
        bitrateMbps = 35.0
        isApplyingRemoteSync = false
        shutterAngle = 180
        isoValue = 400
        wbKelvin = 5600
        exposureBias = 0.0
        zoomFactor = 1.0
        minDisplayZoom = 0.5
        maxDisplayZoom = 10.0
        autoExposure = false
        autoWhiteBalance = false
        autoFocus = false
        aeAfLocked = false
        focusPosition = 0.48
        stabilizationLevel = .cinematic
        activeHardwareStabilization = .cinematic
        supportedStabilizationLevels = StabilizationLevel.allCases
        telemetry.codecBackend = "VideoToolbox H.265 (HW)"
        telemetry.bitrateMbps = 35.2
        telemetry.latencyMs = 7.8
        telemetry.jitterMs = 0.31
        telemetry.lossPct = 0.0
        telemetry.bufferPct = 98
        telemetry.actualFps = 60.0
        statusText = tr("Demo Cine Studio активна • Без підключення ПК", "Demo Cine Studio active • No PC required")
        browser.start(localRole: duoRole, localLabel: cameraLabel, demoMode: true)
        startTelemetryTimer()
    }

    func exitDemoMode() {
        isDemoMode = false
        stopStreaming()
        statusText = tr("Демо завершено • Готово до підключення", "Demo ended • Ready to connect")
        browser.start(localRole: duoRole, localLabel: cameraLabel, demoMode: false)
        startLocalPreviewIfAuthorized()
    }

    func cycleDemoTally() {
        switch tallyState {
        case .standby:
            tallyState = .preview
            remoteRec = false
            directorSpeaking = false
        case .preview:
            tallyState = .program
            remoteRec = true
            directorSpeaking = true
        case .program:
            tallyState = .standby
            remoteRec = false
            directorSpeaking = false
        }
        syncDuoStateToPeers()
    }

    // MARK: - Multi-Level Image Stabilization

    func setStabilizationLevel(_ level: StabilizationLevel) {
        stabilizationLevel = level
        selectionFeedback.selectionChanged()
        if isDemoMode {
            activeHardwareStabilization = level
            showTouchToast(tr("Стабілізація: ", "Stabilization: ") + level.title, icon: level.systemIcon)
            return
        }
        applyStabilizationToActiveSessions(level)
        showTouchToast(tr("Стабілізація: ", "Stabilization: ") + level.title, icon: level.systemIcon)
    }

    func cycleStabilizationLevel() {
        let all = StabilizationLevel.allCases
        if let idx = all.firstIndex(of: stabilizationLevel) {
            let next = all[(idx + 1) % all.count]
            setStabilizationLevel(next)
        }
    }

    private nonisolated static func resolveAVStabilizationMode(
        for requested: StabilizationLevel,
        format: AVCaptureDevice.Format?
    ) -> (mode: AVCaptureVideoStabilizationMode, resolvedLevel: StabilizationLevel) {
        let candidates: [(AVCaptureVideoStabilizationMode, StabilizationLevel)]
        switch requested {
        case .off:
            return (.off, .off)
        case .standard:
            candidates = [(.standard, .standard), (.off, .off)]
        case .cinematic:
            candidates = [(.cinematic, .cinematic), (.standard, .standard), (.off, .off)]
        case .active:
            candidates = [(.cinematicExtended, .active), (.cinematic, .cinematic), (.standard, .standard), (.off, .off)]
        case .ultra:
            if #available(iOS 18.0, *) {
                candidates = [
                    (.cinematicExtendedEnhanced, .ultra),
                    (.cinematicExtended, .active),
                    (.cinematic, .cinematic),
                    (.standard, .standard),
                    (.off, .off)
                ]
            } else {
                candidates = [
                    (.cinematicExtended, .active),
                    (.cinematic, .cinematic),
                    (.standard, .standard),
                    (.off, .off)
                ]
            }
        }
        guard let fmt = format else {
            return candidates.first ?? (.off, .off)
        }
        for (mode, lvl) in candidates {
            if mode == .off || fmt.isVideoStabilizationModeSupported(mode) {
                return (mode, lvl)
            }
        }
        return (.off, .off)
    }

    private nonisolated static func querySupportedStabilizationLevels(format: AVCaptureDevice.Format) -> [StabilizationLevel] {
        var result: [StabilizationLevel] = [.off]
        if format.isVideoStabilizationModeSupported(.standard) { result.append(.standard) }
        if format.isVideoStabilizationModeSupported(.cinematic) { result.append(.cinematic) }
        if format.isVideoStabilizationModeSupported(.cinematicExtended) { result.append(.active) }
        if #available(iOS 18.0, *) {
            if format.isVideoStabilizationModeSupported(.cinematicExtendedEnhanced) {
                result.append(.ultra)
            }
        }
        return result
    }

    private func applyStabilizationToActiveSessions(_ level: StabilizationLevel) {
        let dev = primaryDevice
        captureQueue.async { [weak self, singleSession, multiCamSession] in
            guard let self else { return }
            let resolved = Self.resolveAVStabilizationMode(for: level, format: dev?.activeFormat)
            var appliedLevel = resolved.resolvedLevel

            for out in singleSession.outputs {
                if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                    conn.preferredVideoStabilizationMode = resolved.mode
                    appliedLevel = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolved.resolvedLevel)
                }
            }
            if let mc = multiCamSession {
                for out in mc.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                        conn.preferredVideoStabilizationMode = resolved.mode
                        appliedLevel = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolved.resolvedLevel)
                    }
                }
            }
            let supported = dev.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
            Task { @MainActor in
                self.activeHardwareStabilization = appliedLevel
                self.supportedStabilizationLevels = supported
            }
        }
    }

    private nonisolated static func level(from mode: AVCaptureVideoStabilizationMode, fallback: StabilizationLevel) -> StabilizationLevel {
        switch mode {
        case .off: return .off
        case .standard: return .standard
        case .cinematic: return .cinematic
        case .cinematicExtended: return .active
        case .auto: return fallback
        default:
            if #available(iOS 18.0, *), mode == .cinematicExtendedEnhanced {
                return .ultra
            }
            return fallback
        }
    }

    // MARK: - Intuitive Viewfinder Touch Interactions (Zoom, Focus, Exposure, AE/AF Lock)

    func showTouchToast(_ text: String, icon: String = "viewfinder") {
        touchHUD.toastText = text
        touchHUD.toastIcon = icon
        toastHideTask?.cancel()
        toastHideTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_800_000_000)
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.25)) {
                self?.touchHUD.toastText = nil
            }
        }
    }

    private func scheduleReticleAutoHide(after seconds: Double = 2.8) {
        reticleHideTask?.cancel()
        reticleHideTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            guard !Task.isCancelled, let self else { return }
            if !self.aeAfLocked && !self.touchHUD.adjustingExposure && !self.touchHUD.adjustingFocus {
                withAnimation(.easeOut(duration: 0.30)) {
                    self.touchHUD.visible = false
                }
            }
        }
    }

    /// Single tap anywhere on viewfinder -> focus & exposure metering at exact point.
    func focusAndExpose(atDevicePoint devicePoint: CGPoint, viewNormalizedPoint: CGPoint) {
        lightImpact.impactOccurred()
        aeAfLocked = false
        autoFocus = true
        autoExposure = true
        exposureBias = 0.0

        withAnimation(.spring(response: 0.24, dampingFraction: 0.72)) {
            touchHUD.normalizedPoint = viewNormalizedPoint
            touchHUD.visible = true
            touchHUD.locked = false
            touchHUD.adjustingExposure = false
            touchHUD.adjustingFocus = false
        }
        scheduleReticleAutoHide(after: 3.0)

        if isDemoMode {
            // Focus smoothly toward crisp demo plane near tap depth
            let distFromCenter = hypot(viewNormalizedPoint.x - 0.5, viewNormalizedPoint.y - 0.5)
            focusPosition = Float(max(0.15, min(0.85, 0.48 + (distFromCenter - 0.15) * 0.25)))
            showTouchToast(tr("Фокус і експозиція", "Focus & Exposure"), icon: "viewfinder")
            return
        }

        guard let dev = primaryDevice else { return }
        do {
            try dev.lockForConfiguration()
            if dev.isFocusPointOfInterestSupported {
                dev.focusPointOfInterest = devicePoint
                if dev.isFocusModeSupported(.autoFocus) {
                    dev.focusMode = .autoFocus
                } else if dev.isFocusModeSupported(.continuousAutoFocus) {
                    dev.focusMode = .continuousAutoFocus
                }
            }
            if dev.isExposurePointOfInterestSupported {
                dev.exposurePointOfInterest = devicePoint
                if dev.isExposureModeSupported(.continuousAutoExposure) {
                    dev.exposureMode = .continuousAutoExposure
                } else if dev.isExposureModeSupported(.autoExpose) {
                    dev.exposureMode = .autoExpose
                }
            }
            dev.setExposureTargetBias(0.0, completionHandler: nil)
            dev.isSubjectAreaChangeMonitoringEnabled = true
            dev.unlockForConfiguration()
        } catch {}
    }

    /// Long press anywhere on viewfinder -> AE/AF Lock at point.
    func lockFocusAndExposure(atDevicePoint devicePoint: CGPoint, viewNormalizedPoint: CGPoint) {
        mediumImpact.impactOccurred()
        aeAfLocked = true
        autoFocus = false
        autoExposure = false

        withAnimation(.spring(response: 0.24, dampingFraction: 0.68)) {
            touchHUD.normalizedPoint = viewNormalizedPoint
            touchHUD.visible = true
            touchHUD.locked = true
            touchHUD.adjustingExposure = false
            touchHUD.adjustingFocus = false
        }
        reticleHideTask?.cancel()
        showTouchToast(tr("AE / AF заблоковано", "AE / AF Locked"), icon: "lock.fill")

        if isDemoMode { return }

        guard let dev = primaryDevice else { return }
        do {
            try dev.lockForConfiguration()
            if dev.isFocusPointOfInterestSupported {
                dev.focusPointOfInterest = devicePoint
            }
            if dev.isExposurePointOfInterestSupported {
                dev.exposurePointOfInterest = devicePoint
            }
            if dev.isFocusModeSupported(.locked) {
                dev.focusMode = .locked
                focusPosition = dev.lensPosition
            }
            if dev.isExposureModeSupported(.locked) {
                dev.exposureMode = .locked
            }
            dev.unlockForConfiguration()
        } catch {}
    }

    func unlockAeAf() {
        guard aeAfLocked else { return }
        resetTouchFocusAndExposure()
    }

    /// Vertical drag on viewfinder -> smooth EV exposure compensation (-3.0 ... +3.0 EV).
    func setExposureBias(_ newBias: Float, activeDrag: Bool = false) {
        let clamped = max(-3.0, min(3.0, newBias))
        let nearZero = abs(clamped) < 0.08
        if nearZero && !lastEvHapticZeroCross {
            selectionFeedback.selectionChanged()
        }
        lastEvHapticZeroCross = nearZero

        let snapped = nearZero ? 0.0 : (round(clamped * 10.0) / 10.0)
        exposureBias = snapped
        touchHUD.visible = true
        touchHUD.adjustingExposure = activeDrag

        if !activeDrag {
            scheduleReticleAutoHide(after: 2.4)
        } else {
            reticleHideTask?.cancel()
        }

        let sign = snapped >= 0 ? "+" : ""
        showTouchToast(String(format: "%@ %@%.1f EV", tr("Експозиція", "Exposure"), sign, snapped), icon: "sun.max.fill")

        if isDemoMode { return }

        guard let dev = primaryDevice else { return }
        do {
            try dev.lockForConfiguration()
            if dev.exposureMode == .custom {
                // In custom manual mode, switch seamlessly to AE with target bias so vertical swipe works immediately
                if dev.isExposureModeSupported(.continuousAutoExposure) {
                    dev.exposureMode = .continuousAutoExposure
                    autoExposure = true
                }
            }
            let hwClamped = max(dev.minExposureTargetBias, min(dev.maxExposureTargetBias, snapped))
            dev.setExposureTargetBias(hwClamped, completionHandler: nil)
            dev.unlockForConfiguration()
        } catch {}
    }

    /// Horizontal drag on viewfinder -> smooth Manual Focus Pull (0.0 ... 1.0).
    func setTouchManualFocus(_ position: Float, activeDrag: Bool = false) {
        let clamped = max(0.0, min(1.0, position))
        autoFocus = false
        focusPosition = clamped
        touchHUD.adjustingFocus = activeDrag
        if !activeDrag {
            scheduleReticleAutoHide(after: 2.0)
        } else {
            reticleHideTask?.cancel()
        }
        showTouchToast(String(format: "%@ %d%%", tr("Фокус MF", "Manual Focus"), Int(clamped * 100)), icon: "scope")

        if isDemoMode { return }

        guard let dev = primaryDevice else { return }
        do {
            try dev.lockForConfiguration()
            if dev.isFocusModeSupported(.locked) {
                dev.setFocusModeLocked(lensPosition: clamped, completionHandler: nil)
            }
            dev.unlockForConfiguration()
        } catch {}
    }

    /// Double tap on viewfinder -> instant reset to Auto Focus, Auto Exposure, EV 0.0.
    func resetTouchFocusAndExposure() {
        mediumImpact.impactOccurred()
        aeAfLocked = false
        autoFocus = true
        autoExposure = true
        exposureBias = 0.0

        withAnimation(.easeOut(duration: 0.2)) {
            touchHUD.locked = false
            touchHUD.normalizedPoint = CGPoint(x: 0.5, y: 0.5)
            touchHUD.visible = true
            touchHUD.adjustingExposure = false
            touchHUD.adjustingFocus = false
        }
        scheduleReticleAutoHide(after: 1.4)
        showTouchToast(tr("AF / AE Авто · 0.0 EV", "AF / AE Auto · 0.0 EV"), icon: "arrow.counterclockwise")

        if isDemoMode {
            focusPosition = 0.48
            return
        }

        guard let dev = primaryDevice else { return }
        do {
            try dev.lockForConfiguration()
            if dev.isFocusPointOfInterestSupported { dev.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5) }
            if dev.isFocusModeSupported(.continuousAutoFocus) { dev.focusMode = .continuousAutoFocus }
            if dev.isExposurePointOfInterestSupported { dev.exposurePointOfInterest = CGPoint(x: 0.5, y: 0.5) }
            if dev.isExposureModeSupported(.continuousAutoExposure) { dev.exposureMode = .continuousAutoExposure }
            dev.setExposureTargetBias(0.0, completionHandler: nil)
            dev.unlockForConfiguration()
        } catch {}
    }

    /// Continuous pinch / wheel zoom (display scale 0.5x ... 10.0x, mapped seamlessly to hardware multi-cam).
    func setDisplayZoom(_ requestedDisplayZoom: CGFloat, animated: Bool = false, showOverlay: Bool = true) {
        let clampedDisplay = max(minDisplayZoom, min(maxDisplayZoom, requestedDisplayZoom))
        zoomFactor = clampedDisplay

        // Subtle haptic detent at optical focal steps (0.5x, 1x, 2x, 3x, 5x)
        let quantized = Int(round(clampedDisplay * 10))
        if [5, 10, 20, 30, 50].contains(quantized) && quantized != lastZoomDetent {
            selectionFeedback.selectionChanged()
        }
        lastZoomDetent = quantized

        // Keep lens badge synced with rear display zoom
        if lens != .front {
            if clampedDisplay < 0.85 {
                lens = .ultraWide
            } else if clampedDisplay < 1.75 {
                lens = .wide
            } else if clampedDisplay < 2.65 {
                lens = .tele
            } else {
                lens = .tele3x
            }
        }

        if showOverlay {
            showTouchToast(String(format: "%@ %.1f×", tr("Зум", "Zoom"), clampedDisplay), icon: "magnifyingglass")
        }

        if isDemoMode { return }

        guard let dev = primaryDevice else { return }
        let hwTarget: CGFloat
        if isUsingVirtualRearDevice {
            hwTarget = clampedDisplay * wideBaseZoomFactor
        } else {
            // Discrete lens: map relative to its own optical baseline
            let base = max(0.5, lens.targetDisplayZoom)
            hwTarget = max(1.0, clampedDisplay / base)
        }
        let maxHw = min(dev.activeFormat.videoMaxZoomFactor, maxDisplayZoom * wideBaseZoomFactor)
        let finalHw = max(dev.minAvailableVideoZoomFactor, min(maxHw, hwTarget))
        do {
            try dev.lockForConfiguration()
            if animated {
                dev.ramp(toVideoZoomFactor: finalHw, withRate: 10.0)
            } else {
                dev.cancelVideoZoomRamp()
                dev.videoZoomFactor = finalHw
            }
            dev.unlockForConfiguration()
        } catch {}
    }

    func stepZoom(up: Bool) {
        let steps: [CGFloat] = [0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0, 10.0].filter { $0 >= minDisplayZoom && $0 <= maxDisplayZoom }
        if up {
            if let next = steps.first(where: { $0 > zoomFactor + 0.05 }) {
                setDisplayZoom(next, animated: true)
            }
        } else {
            if let prev = steps.last(where: { $0 < zoomFactor - 0.05 }) {
                setDisplayZoom(prev, animated: true)
            }
        }
    }

    // MARK: - Live Streaming (LanVidCore + VideoToolbox + TallyIntercom)

    func toggleStreaming() {
        if isDemoMode {
            exitDemoMode()
            return
        }
        if isStreaming {
            stopStreaming()
        } else {
            startLiveStreaming()
        }
    }

    func startLiveStreaming() {
        if duoMode != .duoCompanion {
            let vStatus = AVCaptureDevice.authorizationStatus(for: .video)
            if vStatus == .notDetermined {
                AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                    Task { @MainActor in
                        guard let self else { return }
                        if granted {
                            self.startLiveStreamingAuthorized()
                        } else {
                            self.statusText = tr("Надайте доступ до камери в Налаштуваннях", "Enable Camera access in Settings")
                        }
                    }
                }
                return
            } else if vStatus == .denied || vStatus == .restricted {
                statusText = tr("Доступ до камери заборонено", "Camera access denied")
                return
            }
        }
        let aStatus = AVCaptureDevice.authorizationStatus(for: .audio)
        if aStatus == .notDetermined {
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] _ in
                Task { @MainActor in self?.startLiveStreamingAuthorized() }
            }
            return
        }
        startLiveStreamingAuthorized()
    }

    private func startLiveStreamingAuthorized() {
        stopStreaming(keepPreview: true)
        isDemoMode = false

        if let s = lv_sender_create() {
            var cfg = LV_SenderConfig()
            lv_sdk_sender_config_default(&cfg)
            let ipBytes = Array(destIp.utf8.prefix(63))
            withUnsafeMutableBytes(of: &cfg.dest_ip) { raw in
                raw.initializeMemory(as: UInt8.self, repeating: 0)
                for (i, b) in ipBytes.enumerated() { raw[i] = b }
            }
            cfg.dest_port = UInt16(clamping: videoPort)
            cfg.width = resolution.width
            cfg.height = resolution.height
            cfg.fps = Double(fps)
            cfg.codec = codec.lvCodecValue
            cfg.bitrate_kbps = UInt32(max(2000, Int(bitrateMbps * 1000.0)))
            cfg.fec_group_size = fecEnabled ? 6 : 0
            cfg.adaptive_quality = adaptiveQuality ? 1 : 0
            cfg.keyframe_interval = UInt32(max(24, fps))

            let rc = lv_sdk_sender_start(s, &cfg)
            if rc == LV_OK.rawValue {
                senderLock.lock()
                senderHandle = s
                senderLock.unlock()
            } else {
                lv_sender_destroy(s)
                statusText = tr("Помилка запуску потоку (", "Stream start error (") + "\(rc))"
                return
            }
        } else {
            statusText = tr("Не вдалося ініціалізувати відеопотік", "Failed to initialize sender")
            return
        }

        let th = destIp.withCString { ipC in
            cameraLabel.withCString { lblC in
                lv_tally_intercom_create_camera(ipC, UInt16(clamping: tallyPort), cameraId, lblC)
            }
        }
        if let th {
            let ctx = Unmanaged.passUnretained(self).toOpaque()
            lv_camera_set_tally_callback(th, { statePtr, userData in
                guard let statePtr, let userData else { return }
                let st = statePtr.pointee
                let engine = Unmanaged<CameraEngine>.fromOpaque(userData).takeUnretainedValue()
                Task { @MainActor in
                    engine.tallyState = ProTallyState(rawValue: Int(st.tally_state)) ?? .standby
                    engine.remoteRec = (st.flags & 0x01) != 0
                }
            }, ctx)
            lv_camera_set_control_callback(th, { ctrlPtr, userData in
                guard let ctrlPtr, let userData else { return }
                let ctrl = ctrlPtr.pointee
                let engine = Unmanaged<CameraEngine>.fromOpaque(userData).takeUnretainedValue()
                Task { @MainActor in
                    engine.applyRemoteStudioControl(ctrl)
                }
            }, ctx)
            lv_tally_intercom_set_video_config_sync_callback(th, { cfgPtr, userData in
                guard let cfgPtr, let userData else { return }
                let cfg = cfgPtr.pointee
                let engine = Unmanaged<CameraEngine>.fromOpaque(userData).takeUnretainedValue()
                Task { @MainActor in
                    engine.applyRemoteVideoConfigSync(cfg)
                }
            }, ctx)

            tallyLock.lock()
            tallyHandle = th
            tallyLock.unlock()
            syncIntercomState()
            startTallyPollTimer()
        }

        if duoMode != .duoCompanion {
            if !isPreviewActive || !singleSession.isRunning {
                configureAndStartCapture()
                isPreviewActive = true
            }
        }
        intercomAudio.start()
        streamStartWallTime = CACurrentMediaTime()
        isStreaming = true
        UIApplication.shared.isIdleTimerDisabled = true
        statusText = "\(resolution.rawValue) \(fps)p \(codec.shortName) → \(destIp):\(videoPort)"
        startTelemetryTimer()
    }

    func stopStreaming(keepPreview: Bool = true) {
        tickTimer?.invalidate()
        tickTimer = nil
        tallyPollTimer?.cancel()
        tallyPollTimer = nil
        if !keepPreview {
            isPreviewActive = false
            captureQueue.async { [singleSession, multiCamSession] in
                if singleSession.isRunning { singleSession.stopRunning() }
                if let mc = multiCamSession, mc.isRunning { mc.stopRunning() }
            }
        }
        intercomAudio.stop()

        senderLock.lock()
        let oldSender = senderHandle
        senderHandle = nil
        senderLock.unlock()
        if let s = oldSender {
            lv_sender_stop(s)
            lv_sender_destroy(s)
        }

        tallyLock.lock()
        let oldTally = tallyHandle
        tallyHandle = nil
        isMicTalkOpen = false
        tallyLock.unlock()
        if let t = oldTally {
            lv_tally_intercom_destroy(t)
        }

        isStreaming = false
        UIApplication.shared.isIdleTimerDisabled = false
        if !isDemoMode {
            tallyState = .standby
            remoteRec = false
            directorSpeaking = false
        }
    }

    private func startTallyPollTimer() {
        tallyPollTimer?.cancel()
        let timer = DispatchSource.makeTimerSource(queue: tallyPollQueue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(2))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.tallyLock.lock()
            var speaking = false
            var duck: Float = 1.0
            if let th = self.tallyHandle {
                lv_tally_intercom_poll(th)
                speaking = (lv_tally_intercom_is_director_speaking(th) != 0)
                duck = lv_tally_intercom_ducking_gain(th)
            }
            self.tallyLock.unlock()
            Task { @MainActor in
                if self.directorSpeaking != speaking { self.directorSpeaking = speaking }
                if abs(self.duckingGain - duck) > 0.01 { self.duckingGain = duck }
            }
        }
        timer.resume()
        tallyPollTimer = timer
    }

    // MARK: - iPhone Duo Mode Switching & Companion Remote Control

    func setDuoMode(_ newMode: DuoMode) {
        duoMode = newMode
        if newMode == .duoCompanion {
            duoRole = .companion
        } else if duoRole == .companion {
            duoRole = .camA
        }
        if (isStreaming || isPreviewActive) && !isDemoMode {
            if newMode == .duoCompanion {
                captureQueue.async { [singleSession, multiCamSession] in
                    if singleSession.isRunning { singleSession.stopRunning() }
                    if let mc = multiCamSession, mc.isRunning { mc.stopRunning() }
                }
            } else {
                configureAndStartCapture()
            }
        }
        syncDuoStateToPeers()
    }

    func sendCompanionCommandToSelectedPeer() {
        guard let host = selectedDuoPeerHost ?? browser.duoPeers.first?.host else { return }
        browser.sendDuoPacket(
            to: host,
            role: duoRole,
            label: cameraLabel,
            tally: tallyState,
            timecode: telemetry.timecode,
            focus: focusPosition,
            iso: isoValue,
            shutter: shutterAngle,
            wb: wbKelvin,
            isCommand: true
        )
    }

    func syncDuoStateToPeers() {
        for peer in browser.duoPeers {
            browser.sendDuoPacket(
                to: peer.host,
                role: duoRole,
                label: cameraLabel,
                tally: tallyState,
                timecode: telemetry.timecode,
                focus: focusPosition,
                iso: isoValue,
                shutter: shutterAngle,
                wb: wbKelvin,
                isCommand: false
            )
        }
    }

    // MARK: - Manual Camera Deck Controls

    func rackFocus(to target: Float) {
        autoFocus = false
        focusPosition = target
        applyManualCameraControls()
        showTouchToast(String(format: "Rack Focus → %d%%", Int(target * 100)), icon: "scope")
        if duoMode == .duoCompanion { sendCompanionCommandToSelectedPeer() }
    }

    func selectLens(_ newLens: CineLens) {
        let wasFront = (lens == .front)
        let wantsFront = (newLens == .front)
        lens = newLens
        selectionFeedback.selectionChanged()

        if isDemoMode {
            zoomFactor = newLens.targetDisplayZoom
            showTouchToast(tr("Об'єктив: ", "Lens: ") + newLens.displayLabel, icon: "camera.aperture")
            return
        }

        // Fast seamless zoom ramp if already on a virtual rear multi-camera device
        if !wantsFront && !wasFront && isUsingVirtualRearDevice && !duoMode.isMultiCam && (isStreaming || isPreviewActive) {
            setDisplayZoom(newLens.targetDisplayZoom, animated: true, showOverlay: true)
            if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() }
            return
        }

        zoomFactor = newLens.targetDisplayZoom
        if (isStreaming || isPreviewActive) && !duoMode.isMultiCam {
            configureAndStartCapture()
        }
        if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() }
    }

    func applyManualCameraControls() {
        if let dev = primaryDevice {
            applyExposureAndFocus(to: dev, autoExp: autoExposure, shutter: shutterAngle, iso: isoValue,
                                  autoWB: autoWhiteBalance, kelvin: wbKelvin, tint: wbTint,
                                  autoFoc: autoFocus, focusPos: focusPosition)
        }
        if let sec = secondaryDevice {
            do {
                try sec.lockForConfiguration()
                if secondaryAutoExposure {
                    if sec.isExposureModeSupported(.continuousAutoExposure) {
                        sec.exposureMode = .continuousAutoExposure
                    }
                    let clampedBias = max(sec.minExposureTargetBias, min(sec.maxExposureTargetBias, secondaryExposureBias))
                    sec.setExposureTargetBias(clampedBias, completionHandler: nil)
                }
                sec.unlockForConfiguration()
            } catch {}
        }
        if duoMode == .duoCompanion {
            sendCompanionCommandToSelectedPeer()
        }
    }

    private func applyExposureAndFocus(to dev: AVCaptureDevice, autoExp: Bool, shutter: Int, iso: Float,
                                       autoWB: Bool, kelvin: Float, tint: Float,
                                       autoFoc: Bool, focusPos: Float) {
        do {
            try dev.lockForConfiguration()
            if autoExp {
                if dev.isExposureModeSupported(.continuousAutoExposure) { dev.exposureMode = .continuousAutoExposure }
                let clampedBias = max(dev.minExposureTargetBias, min(dev.maxExposureTargetBias, exposureBias))
                dev.setExposureTargetBias(clampedBias, completionHandler: nil)
            } else if dev.isExposureModeSupported(.custom) {
                let denom = Double(max(1, fps)) * (360.0 / Double(max(1, shutter)))
                let rawDur = CMTime(value: 1, timescale: CMTimeScale(max(24, Int(denom))))
                let minDur = dev.activeFormat.minExposureDuration
                let maxDur = dev.activeFormat.maxExposureDuration
                let clampedDur = CMTimeMinimum(maxDur, CMTimeMaximum(minDur, rawDur))
                let clampedIso = max(dev.activeFormat.minISO, min(dev.activeFormat.maxISO, iso))
                dev.setExposureModeCustom(duration: clampedDur, iso: clampedIso)
            }
            if autoWB {
                if dev.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) { dev.whiteBalanceMode = .continuousAutoWhiteBalance }
            } else if dev.isWhiteBalanceModeSupported(.locked) {
                let tt = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(temperature: kelvin, tint: tint)
                let gains = dev.deviceWhiteBalanceGains(for: tt)
                let clamped = AVCaptureDevice.WhiteBalanceGains(
                    redGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.redGain)),
                    greenGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.greenGain)),
                    blueGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.blueGain))
                )
                dev.setWhiteBalanceModeLocked(with: clamped)
            }
            if autoFoc {
                if dev.isFocusModeSupported(.continuousAutoFocus) { dev.focusMode = .continuousAutoFocus }
            } else if dev.isFocusModeSupported(.locked) {
                dev.setFocusModeLocked(lensPosition: max(0.0, min(1.0, focusPos)))
            }
            dev.unlockForConfiguration()
        } catch {}
    }

    private func applyRemoteStudioControl(_ ctrl: LV_CameraControl) {
        isApplyingRemoteSync = true
        defer { isApplyingRemoteSync = false }

        let targetLens: CineLens = {
            switch ctrl.lens {
            case 1: return .ultraWide
            case 2: return .tele
            case 3: return .front
            default: return .wide
            }
        }()
        if targetLens != lens {
            selectLens(targetLens)
        }

        let targetRes: CineResolution = {
            switch ctrl.quality {
            case 0, 1: return .hd720
            case 3: return .uhd4k
            default: return .fhd1080
            }
        }()
        if targetRes != resolution {
            resolution = targetRes
            if isStreaming && !isDemoMode {
                reconfigureVideoSenderOnly()
            }
        }

        autoFocus = (ctrl.focus_auto != 0)
        if !autoFocus {
            focusPosition = Float(min(1000, ctrl.focus_pos)) / 1000.0
        }
        applyManualCameraControls()

        if ctrl.zoom_x100 >= 100 {
            let requestedZoom = CGFloat(ctrl.zoom_x100) / 100.0
            setDisplayZoom(requestedZoom, animated: true, showOverlay: true)
        }
    }

    private func applyRemoteVideoConfigSync(_ cfg: LV_VideoConfigSync) {
        if (cfg.flags & 0x01) != 0 {
            senderLock.lock()
            let s = senderHandle
            if let s { lv_sender_force_keyframe(s) }
            senderLock.unlock()
        }

        isApplyingRemoteSync = true
        defer { isApplyingRemoteSync = false }

        if cfg.assigned_cam_number > 0 {
            assignedCamNumber = Int(cfg.assigned_cam_number)
        }
        let remoteTag = withUnsafePointer(to: cfg.assigned_cam_tag) {
            $0.withMemoryRebound(to: CChar.self, capacity: 16) { String(cString: $0) }
        }
        if !remoteTag.isEmpty {
            assignedCamTag = remoteTag
            cameraLabel = remoteTag
        } else if cfg.assigned_cam_number > 0 {
            assignedCamTag = "CAM \(cfg.assigned_cam_number)"
            cameraLabel = assignedCamTag
        }

        if cfg.video_delay_ms >= 5 {
            syncedDelayMs = Int(cfg.video_delay_ms)
        }
        syncedFixedDelay = (cfg.fixed_delay_mode != 0)
        configSynced = true

        let targetRes: CineResolution = {
            switch cfg.resolution_preset {
            case 0, 1: return .hd720
            case 3: return .uhd4k
            default: return .fhd1080
            }
        }()
        let targetCodec: CineCodec = {
            switch cfg.codec {
            case 0: return .dct
            case 1: return .h264
            default: return .hevc
            }
        }()
        let targetLens: CineLens = {
            switch cfg.camera_lens {
            case 1: return .ultraWide
            case 2: return .tele
            case 3: return .front
            default: return .wide
            }
        }()
        let targetFps = cfg.target_fps >= 15 ? Int(cfg.target_fps) : fps
        let targetMbps = cfg.bitrate_kbps >= 1000 ? Double(cfg.bitrate_kbps) / 1000.0 : bitrateMbps

        let needEncoderRestart = (targetRes != resolution || targetCodec != codec || targetFps != fps)
        let needLensSwitch = (targetLens != lens)

        resolution = targetRes
        codec = targetCodec
        fps = targetFps
        bitrateMbps = min(100.0, max(5.0, targetMbps))

        if needLensSwitch {
            selectLens(targetLens)
        }
        if needEncoderRestart && isStreaming && !isDemoMode {
            reconfigureVideoSenderOnly()
        } else if isStreaming && !isDemoMode {
            senderLock.lock()
            let s = senderHandle
            if let s { lv_sender_set_bitrate(s, UInt32(max(2000, Int(bitrateMbps * 1000.0)))) }
            senderLock.unlock()
        }
    }

    private func reconfigureVideoSenderOnly() {
        guard let s = lv_sender_create() else { return }
        var cfg = LV_SenderConfig()
        lv_sdk_sender_config_default(&cfg)
        let ipBytes = Array(destIp.utf8.prefix(63))
        withUnsafeMutableBytes(of: &cfg.dest_ip) { raw in
            raw.initializeMemory(as: UInt8.self, repeating: 0)
            for (i, b) in ipBytes.enumerated() { raw[i] = b }
        }
        cfg.dest_port = UInt16(clamping: videoPort)
        cfg.width = resolution.width
        cfg.height = resolution.height
        cfg.fps = Double(fps)
        cfg.codec = codec.lvCodecValue
        cfg.bitrate_kbps = UInt32(max(2000, Int(bitrateMbps * 1000.0)))
        cfg.fec_group_size = fecEnabled ? 6 : 0
        cfg.adaptive_quality = adaptiveQuality ? 1 : 0
        cfg.keyframe_interval = UInt32(max(24, fps))

        if lv_sdk_sender_start(s, &cfg) == LV_OK.rawValue {
            senderLock.lock()
            let old = senderHandle
            senderHandle = s
            senderLock.unlock()
            if let old {
                lv_sender_stop(old)
                lv_sender_destroy(old)
            }
            if duoMode != .duoCompanion {
                configureAndStartCapture()
            }
            statusText = "\(resolution.rawValue) \(fps)p \(codec.shortName) → \(destIp):\(videoPort)"
        } else {
            lv_sender_destroy(s)
        }
    }

    func sendLocalConfigSyncToStudio() {
        localConfigSyncRev &+= 1
        var cfg = LV_VideoConfigSync()
        cfg.config_revision = localConfigSyncRev
        cfg.camera_id = cameraId
        cfg.assigned_cam_number = UInt8(clamping: max(0, min(16, assignedCamNumber)))
        cfg.resolution_preset = {
            switch resolution {
            case .hd720: return 1
            case .fhd1080: return 2
            case .uhd4k: return 3
            }
        }()
        cfg.target_fps = UInt8(clamping: fps)
        cfg.codec = UInt8(clamping: codec.lvCodecValue)
        cfg.bitrate_kbps = UInt32(max(2000, Int(bitrateMbps * 1000.0)))
        cfg.video_delay_ms = UInt16(clamping: max(5, min(250, syncedDelayMs)))
        cfg.fixed_delay_mode = syncedFixedDelay ? 1 : 0
        cfg.camera_lens = {
            switch lens {
            case .ultraWide: return 1
            case .tele, .tele3x: return 2
            case .front: return 3
            case .wide: return 0
            }
        }()
        cfg.flags = 0x02
        let tagBytes = Array(displayBadgeTag.utf8.prefix(15))
        withUnsafeMutableBytes(of: &cfg.assigned_cam_tag) { raw in
            raw.initializeMemory(as: UInt8.self, repeating: 0)
            for (i, b) in tagBytes.enumerated() { raw[i] = b }
        }
        tallyLock.lock()
        if let th = tallyHandle {
            _ = lv_tally_intercom_send_video_config_sync(th, &cfg)
        }
        tallyLock.unlock()
    }

    private func syncIntercomState() {
        let talkOpen = pttActive || latchActive
        let listenOpen = ifbEnabled
        let vol = ifbVolume
        tallyLock.lock()
        isMicTalkOpen = talkOpen
        isIfbListenOpen = listenOpen
        if let th = tallyHandle {
            lv_intercom_set_state(th, talkOpen ? 1 : 0, listenOpen ? 0 : 1, vol, 0.0)
        }
        tallyLock.unlock()
    }

    // MARK: - AVFoundation Capture Setup (Single Virtual Multi-Lens + AVCaptureMultiCamSession)

    private func currentVideoOrientation() -> AVCaptureVideoOrientation {
        let scene = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first(where: { $0.activationState == .foregroundActive })
            ?? UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }).first
        switch scene?.interfaceOrientation {
        case .landscapeLeft: return .landscapeLeft
        case .landscapeRight: return .landscapeRight
        case .portraitUpsideDown: return .portraitUpsideDown
        default: return .portrait
        }
    }

    private func syncCaptureConnectionOrientation() {
        let orient = currentVideoOrientation()
        captureQueue.async { [singleSession, multiCamSession] in
            for out in singleSession.outputs {
                if let conn = out.connection(with: .video), conn.isVideoOrientationSupported {
                    conn.videoOrientation = orient
                }
            }
            if let mc = multiCamSession {
                for out in mc.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoOrientationSupported {
                        conn.videoOrientation = orient
                    }
                }
            }
        }
    }

    private nonisolated static func applyFrameRate(_ targetFps: Int, to dev: AVCaptureDevice) {
        let desired = Double(max(15, targetFps))
        let supported = dev.activeFormat.videoSupportedFrameRateRanges.contains {
            $0.minFrameRate <= desired && desired <= $0.maxFrameRate
        }
        guard supported else { return }
        let dur = CMTime(value: 1, timescale: CMTimeScale(targetFps))
        do {
            try dev.lockForConfiguration()
            dev.activeVideoMinFrameDuration = dur
            dev.activeVideoMaxFrameDuration = dur
            dev.unlockForConfiguration()
        } catch {}
    }

    private nonisolated static func selectBestRearDevice() -> (device: AVCaptureDevice?, isVirtual: Bool, wideBase: CGFloat, minDisp: CGFloat) {
        if let triple = AVCaptureDevice.default(.builtInTripleCamera, for: .video, position: .back) {
            let sw = CGFloat(triple.virtualDeviceSwitchOverVideoZoomFactors.first?.doubleValue ?? 2.0)
            return (triple, true, max(1.0, sw), 1.0 / max(1.0, sw))
        }
        if let dualWide = AVCaptureDevice.default(.builtInDualWideCamera, for: .video, position: .back) {
            let sw = CGFloat(dualWide.virtualDeviceSwitchOverVideoZoomFactors.first?.doubleValue ?? 2.0)
            return (dualWide, true, max(1.0, sw), 1.0 / max(1.0, sw))
        }
        if let dual = AVCaptureDevice.default(.builtInDualCamera, for: .video, position: .back) {
            return (dual, true, 1.0, 1.0)
        }
        let wide = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        return (wide, false, 1.0, 1.0)
    }

    private func configureAndStartCapture() {
        let wantMultiCam = duoMode.isMultiCam && multiCamSession != nil
        let targetRes = resolution
        let targetLens = lens
        let targetFps = fps
        let targetZoom = zoomFactor
        let targetStab = stabilizationLevel
        let targetOrientation = currentVideoOrientation()

        captureQueue.async { [weak self] in
            guard let self else { return }
            if wantMultiCam, let mc = self.multiCamSession {
                if self.singleSession.isRunning { self.singleSession.stopRunning() }
                mc.beginConfiguration()
                for i in mc.inputs { mc.removeInput(i) }
                for o in mc.outputs { mc.removeOutput(o) }

                let backCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
                let frontCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
                var appliedStab: StabilizationLevel = .off
                if let backCam, let bIn = try? AVCaptureDeviceInput(device: backCam), mc.canAddInput(bIn) {
                    mc.addInputWithNoConnections(bIn)
                    let out = AVCaptureVideoDataOutput()
                    out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
                    out.alwaysDiscardsLateVideoFrames = true
                    out.setSampleBufferDelegate(self, queue: self.captureQueue)
                    if mc.canAddOutput(out) {
                        mc.addOutputWithNoConnections(out)
                        if let port = bIn.ports(for: .video, sourceDeviceType: backCam.deviceType, sourceDevicePosition: .back).first {
                            let conn = AVCaptureConnection(inputPorts: [port], output: out)
                            if conn.isVideoOrientationSupported { conn.videoOrientation = targetOrientation }
                            if conn.isVideoStabilizationSupported {
                                let res = Self.resolveAVStabilizationMode(for: targetStab, format: backCam.activeFormat)
                                conn.preferredVideoStabilizationMode = res.mode
                                appliedStab = Self.level(from: conn.activeVideoStabilizationMode, fallback: res.resolvedLevel)
                            }
                            if mc.canAddConnection(conn) { mc.addConnection(conn) }
                        }
                    }
                }
                if let frontCam, let fIn = try? AVCaptureDeviceInput(device: frontCam), mc.canAddInput(fIn) {
                    mc.addInputWithNoConnections(fIn)
                }
                mc.commitConfiguration()
                if let backCam { Self.applyFrameRate(min(targetFps, 30), to: backCam) }
                mc.startRunning()
                let supportedStab = backCam.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
                Task { @MainActor in
                    self.primaryDevice = backCam
                    self.secondaryDevice = frontCam
                    self.isUsingVirtualRearDevice = false
                    self.wideBaseZoomFactor = 1.0
                    self.minDisplayZoom = 1.0
                    self.maxDisplayZoom = 6.0
                    self.activeHardwareStabilization = appliedStab
                    self.supportedStabilizationLevels = supportedStab
                    self.applyManualCameraControls()
                }
            } else {
                if let mc = self.multiCamSession, mc.isRunning { mc.stopRunning() }
                self.singleSession.beginConfiguration()
                for i in self.singleSession.inputs { self.singleSession.removeInput(i) }

                let cam: AVCaptureDevice?
                let isVirtual: Bool
                let wideBase: CGFloat
                let minDisp: CGFloat

                if targetLens == .front {
                    cam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
                    isVirtual = false
                    wideBase = 1.0
                    minDisp = 1.0
                } else {
                    let pick = Self.selectBestRearDevice()
                    cam = pick.device
                    isVirtual = pick.isVirtual
                    wideBase = pick.wideBase
                    minDisp = pick.minDisp
                }

                if let cam, let input = try? AVCaptureDeviceInput(device: cam), self.singleSession.canAddInput(input) {
                    self.singleSession.addInput(input)
                }
                let desiredPreset: AVCaptureSession.Preset = {
                    switch targetRes {
                    case .uhd4k: return .hd4K3840x2160
                    case .fhd1080: return .hd1920x1080
                    case .hd720: return .hd1280x720
                    }
                }()
                if self.singleSession.canSetSessionPreset(desiredPreset) {
                    self.singleSession.sessionPreset = desiredPreset
                } else if self.singleSession.canSetSessionPreset(.hd1920x1080) {
                    self.singleSession.sessionPreset = .hd1920x1080
                } else if self.singleSession.canSetSessionPreset(.hd1280x720) {
                    self.singleSession.sessionPreset = .hd1280x720
                }
                if self.singleSession.outputs.isEmpty {
                    let out = AVCaptureVideoDataOutput()
                    out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
                    out.alwaysDiscardsLateVideoFrames = true
                    out.setSampleBufferDelegate(self, queue: self.captureQueue)
                    if self.singleSession.canAddOutput(out) { self.singleSession.addOutput(out) }
                }

                var appliedStab: StabilizationLevel = .off
                let resolvedStab = Self.resolveAVStabilizationMode(for: targetStab, format: cam?.activeFormat)
                for out in self.singleSession.outputs {
                    if let conn = out.connection(with: .video) {
                        if conn.isVideoOrientationSupported {
                            conn.videoOrientation = targetOrientation
                        }
                        if conn.isVideoStabilizationSupported {
                            conn.preferredVideoStabilizationMode = resolvedStab.mode
                            appliedStab = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolvedStab.resolvedLevel)
                        }
                    }
                }
                self.singleSession.commitConfiguration()

                if let cam {
                    Self.applyFrameRate(targetFps, to: cam)
                    let desiredHwZoom = isVirtual ? (max(minDisp, targetZoom) * wideBase) : max(1.0, targetZoom)
                    let clampedHwZoom = max(cam.minAvailableVideoZoomFactor, min(min(cam.activeFormat.videoMaxZoomFactor, wideBase * 10.0), desiredHwZoom))
                    do {
                        try cam.lockForConfiguration()
                        cam.videoZoomFactor = clampedHwZoom
                        cam.unlockForConfiguration()
                    } catch {}
                }
                if !self.singleSession.isRunning {
                    self.singleSession.startRunning()
                }
                let supportedStab = cam.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
                let maxDisp = cam.map { min(10.0, max(2.0, $0.activeFormat.videoMaxZoomFactor / wideBase)) } ?? 10.0
                Task { @MainActor in
                    self.primaryDevice = cam
                    self.secondaryDevice = nil
                    self.isUsingVirtualRearDevice = isVirtual
                    self.wideBaseZoomFactor = wideBase
                    self.minDisplayZoom = minDisp
                    self.maxDisplayZoom = maxDisp
                    self.activeHardwareStabilization = appliedStab
                    self.supportedStabilizationLevels = supportedStab
                    self.applyManualCameraControls()
                }
            }
        }
    }

    private func updateThermalTemperature() {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: telemetry.tempC = 34
        case .fair: telemetry.tempC = 40
        case .serious: telemetry.tempC = 46
        case .critical: telemetry.tempC = 52
        @unknown default: telemetry.tempC = 36
        }
    }

    // MARK: - Telemetry Timer

    private func startTelemetryTimer() {
        tickTimer?.invalidate()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.onTelemetryTick() }
        }
    }

    private func onTelemetryTick() {
        let elapsed = max(0.0, CACurrentMediaTime() - streamStartWallTime)
        let wholeSecs = Int(elapsed) + 3600
        let hh = (wholeSecs / 3600) % 24
        let mm = (wholeSecs / 60) % 60
        let ss = wholeSecs % 60
        let frac = elapsed - floor(elapsed)
        let ff = min(max(0, fps - 1), Int(frac * Double(max(1, fps))))
        telemetry.timecode = String(format: "%02d:%02d:%02d:%02d", hh, mm, ss, ff)

        let rawBat = UIDevice.current.batteryLevel
        telemetry.batteryPct = rawBat >= 0 ? Int(rawBat * 100) : 92
        updateThermalTemperature()

        if isDemoMode {
            demoPhaseCounter += 1
            let phase = Double(demoPhaseCounter) * 0.24
            telemetry.bitrateMbps = max(2.0, bitrateMbps + sin(phase) * 0.9)
            let stabAdded = Double(stabilizationLevel.latencyHintMs) * 0.15
            telemetry.latencyMs = 7.4 + stabAdded + cos(phase * 0.7) * 0.5
            telemetry.jitterMs = 0.28 + abs(sin(phase * 0.5)) * 0.12
            telemetry.lossPct = 0.0
            telemetry.bufferPct = 98
            telemetry.actualFps = Double(fps)
            telemetry.micPeakDbfs = (pttActive || latchActive) ? Float(-10.0 + sin(phase * 2.2) * 7.0) : Float(-44.0 + sin(phase) * 4.0)
            telemetry.ifbPeakDbfs = (ifbEnabled && directorSpeaking) ? Float(-12.0 + cos(phase * 1.9) * 6.0) : Float(-48.0 + cos(phase) * 3.0)
            telemetry.histogram = (0..<24).map { i in
                let x = Double(i) / 23.0
                let shift = Double(exposureBias) * 0.09
                let bell = exp(-pow((x - (0.48 + shift)) * 3.1, 2.0)) * 0.82
                let ripple = sin(phase + Double(i) * 0.45) * 0.06
                return Float(max(0.05, min(0.98, bell + ripple)))
            }
            appendTelemetryHistory(bitrate: telemetry.bitrateMbps, latency: telemetry.latencyMs, dropout: false)
            return
        }

        senderLock.lock()
        var st = LV_StreamStats()
        var gotStats = false
        if let s = senderHandle {
            gotStats = (lv_sender_get_stream_stats(s, &st) == LV_OK.rawValue)
        }
        senderLock.unlock()

        if gotStats {
            telemetry.bitrateMbps = st.bitrate_mbps
            telemetry.latencyMs = st.encode_ms
            telemetry.actualFps = st.fps
            telemetry.codecBackend = withUnsafePointer(to: st.codec_backend) {
                $0.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
            }
            appendTelemetryHistory(bitrate: st.bitrate_mbps, latency: st.encode_ms, dropout: st.fps < Double(fps) * 0.85)
        }

        tallyLock.lock()
        if let th = tallyHandle {
            lv_tally_intercom_update_camera_telemetry(th, UInt8(clamping: telemetry.batteryPct), Int8(clamping: telemetry.tempC))
        }
        tallyLock.unlock()
    }

    private func appendTelemetryHistory(bitrate: Double, latency: Double, dropout: Bool) {
        telemetry.history.append(TelemetrySample(bitrateMbps: bitrate, latencyMs: latency, dropout: dropout))
        if telemetry.history.count > 42 {
            telemetry.history.removeFirst(telemetry.history.count - 42)
        }
    }
}

extension CameraEngine: AVCaptureVideoDataOutputSampleBufferDelegate {
    nonisolated func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let w = Int32(CVPixelBufferGetWidth(pb))
        let h = Int32(CVPixelBufferGetHeight(pb))
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let tsUs = UInt32(max(0, Int64(CMTimeGetSeconds(pts) * 1_000_000.0)) & 0xFFFFFFFF)

        senderLock.lock()
        defer { senderLock.unlock() }
        guard let s = senderHandle else { return }

        let nativeRc = lv_sender_push_cvpixelbuffer(s, Unmanaged.passUnretained(pb).toOpaque(), w, h, tsUs)
        if nativeRc == LV_ERR_UNSUPPORTED.rawValue {
            CVPixelBufferLockBaseAddress(pb, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
            guard let p0 = CVPixelBufferGetBaseAddressOfPlane(pb, 0)?.assumingMemoryBound(to: UInt8.self),
                  let p1 = CVPixelBufferGetBaseAddressOfPlane(pb, 1)?.assumingMemoryBound(to: UInt8.self) else { return }
            let s0 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 0))
            let s1 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 1))
            _ = lv_sender_push_cvpixelbuffer_planes(s, p0, s0, p1, s1, w, h, Int32(LV_PIXEL_FORMAT_NV12.rawValue), tsUs)
        }
    }
}
