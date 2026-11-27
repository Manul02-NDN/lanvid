@preconcurrency import AVFoundation
import Foundation
import LanVidCore
import SwiftUI
import UIKit

/// Crash-proof, non-blocking Duplex Intercom audio controller.
/// - Runs ALL AVAudioSession & AVAudioEngine lifecycle work on a dedicated serial `audioQueue` (never blocks Main Thread).
/// - Uses `format: nil` on `installTap` with lazy format conversion + explicit `hasTap` tracking to prevent `InstallTapOnNode` NSExceptions.
/// - Filters `routeChangeNotification` to genuine hardware device changes only (prevents `.categoryChange` infinite restart loops).
/// - Reports peak audio levels via a tiny non-blocking lock-guarded float (ZERO Task/MainActor allocations on RT audio threads).
final class IntercomAudioController {
    private let audioQueue = DispatchQueue(label: "com.manul02.lanvid.camera.intercom.audio", qos: .userInitiated)
    private let audioEngine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?
    private var isRunning = false
    private var shouldRun = false
    private var hasInstalledTap = false
    private var routeDebounceWork: DispatchWorkItem?
    private var observers: [NSObjectProtocol] = []

    // Lock-guarded peak dBFS & mic gain (zero MainActor wakeups from RT audio callbacks!)
    private let levelLock = NSLock()
    private var _micPeakDbfs: Float = -60.0
    private var _ifbPeakDbfs: Float = -60.0
    private var _micGain: Float = 1.0

    var micGain: Float {
        get {
            levelLock.lock()
            let v = _micGain
            levelLock.unlock()
            return v
        }
        set {
            levelLock.lock()
            _micGain = max(0.0, min(2.0, newValue))
            levelLock.unlock()
        }
    }

    var micPeakDbfs: Float {
        levelLock.lock()
        let v = _micPeakDbfs
        levelLock.unlock()
        return v
    }

    var ifbPeakDbfs: Float {
        levelLock.lock()
        let v = _ifbPeakDbfs
        levelLock.unlock()
        return v
    }

    var onMicFloat: ((UnsafePointer<Float>, Int) -> Void)?
    var onPullIfbFloat: ((UnsafeMutablePointer<Float>, Int) -> Bool)?

    init() {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: nil) { [weak self] note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            self?.audioQueue.async {
                self?.handleInterruptionOnQueue(rawType: raw)
            }
        })
        observers.append(nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: nil) { [weak self] note in
            let rawReason = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt
            self?.audioQueue.async {
                self?.handleRouteChangeOnQueue(rawReason: rawReason)
            }
        })
    }

    deinit {
        for obs in observers { NotificationCenter.default.removeObserver(obs) }
        routeDebounceWork?.cancel()
        stopSyncOnDeinit()
    }

    func start() {
        audioQueue.async { [weak self] in
            guard let self else { return }
            self.shouldRun = true
            self.startOnQueue()
        }
    }

    func ensureStarted() {
        audioQueue.async { [weak self] in
            guard let self else { return }
            self.shouldRun = true
            if !self.isRunning {
                self.startOnQueue()
            }
        }
    }

    func stop() {
        audioQueue.async { [weak self] in
            guard let self else { return }
            self.shouldRun = false
            self.routeDebounceWork?.cancel()
            self.routeDebounceWork = nil
            self.cleanupOnQueue(deactivateSession: true)
        }
    }

    private func stopSyncOnDeinit() {
        shouldRun = false
        if hasInstalledTap {
            audioEngine.inputNode.removeTap(onBus: 0)
            hasInstalledTap = false
        }
        if isRunning {
            audioEngine.stop()
            isRunning = false
        }
        if let node = sourceNode {
            audioEngine.detach(node)
            sourceNode = nil
        }
    }

    private func startOnQueue() {
        guard !isRunning else { return }
        cleanupOnQueue()

        let session = AVAudioSession.sharedInstance()
        let micGranted: Bool = {
            if #available(iOS 17.0, *) {
                return AVAudioApplication.shared.recordPermission == .granted ||
                       AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
            } else {
                return session.recordPermission == .granted ||
                       AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
            }
        }()

        // Progressive fallback chain so Intercom NEVER dies silently if .voiceChat or mic hardware conflicts:
        // 1) Full-duplex .playAndRecord + .voiceChat (hardware AEC)
        // 2) Full-duplex .playAndRecord + .default
        // 3) Playback-only .playback (Director IFB still heard even if mic unavailable)
        let attempts: [(AVAudioSession.Category, AVAudioSession.Mode, AVAudioSession.CategoryOptions, Bool)] = micGranted
            ? [
                (.playAndRecord, .voiceChat, [.defaultToSpeaker, .allowBluetoothHFP, .mixWithOthers], true),
                (.playAndRecord, .default, [.defaultToSpeaker, .mixWithOthers], true),
                (.playback, .default, [.mixWithOthers], false)
            ]
            : [
                (.playback, .default, [.mixWithOthers], false)
            ]

        for (category, mode, options, wantRecord) in attempts {
            if startAttemptOnQueue(session: session, category: category, mode: mode, options: options, canRecord: wantRecord) {
                return
            }
            cleanupOnQueue()
        }
    }

    private func startAttemptOnQueue(
        session: AVAudioSession,
        category: AVAudioSession.Category,
        mode: AVAudioSession.Mode,
        options: AVAudioSession.CategoryOptions,
        canRecord: Bool
    ) -> Bool {
        do {
            try session.setCategory(category, mode: mode, options: options)
            try? session.setPreferredSampleRate(48_000)
            try? session.setPreferredIOBufferDuration(0.010)
            try session.setActive(true, options: [])

            // In .voiceChat mode without external headphones/Bluetooth, iOS can default to the quiet earpiece.
            // Explicitly route to loudspeaker when the only output is the built-in receiver!
            if canRecord {
                let outputs = session.currentRoute.outputs
                let onlyEarpiece = !outputs.isEmpty && outputs.allSatisfy { $0.portType == .builtInReceiver }
                if onlyEarpiece {
                    try? session.overrideOutputAudioPort(.speaker)
                }
            }

            guard let targetFmt = AVAudioFormat(
                commonFormat: .pcmFormatFloat32,
                sampleRate: 48_000,
                channels: 1,
                interleaved: false
            ) else { return false }

            // Attach IFB playback source node at 48 kHz mono Float32
            let node = AVAudioSourceNode(format: targetFmt) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
                let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
                let count = Int(frameCount)
                guard let dst = abl.first?.mData?.assumingMemoryBound(to: Float.self), count > 0 else {
                    return noErr
                }
                let gotAudio = self?.onPullIfbFloat?(dst, count) ?? false
                if !gotAudio {
                    memset(dst, 0, count * MemoryLayout<Float>.size)
                    if let self, self.levelLock.try() {
                        self._ifbPeakDbfs = -60.0
                        self.levelLock.unlock()
                    }
                    return noErr
                }
                var peak: Float = 0.00001
                for i in 0..<count {
                    let a = abs(dst[i])
                    if a > peak { peak = a }
                }
                if let self, self.levelLock.try() {
                    self._ifbPeakDbfs = max(-60.0, 20.0 * log10(peak))
                    self.levelLock.unlock()
                }
                return noErr
            }
            audioEngine.attach(node)
            audioEngine.connect(node, to: audioEngine.mainMixerNode, format: targetFmt)
            audioEngine.mainMixerNode.outputVolume = 1.0
            sourceNode = node

            if canRecord {
                let input = audioEngine.inputNode
                let inFmt = input.inputFormat(forBus: 0)
                let outFmt = input.outputFormat(forBus: 0)
                if inFmt.sampleRate > 0 && inFmt.channelCount > 0 && outFmt.sampleRate > 0 && outFmt.channelCount > 0 {
                    input.removeTap(onBus: 0)
                    var cachedConverter: AVAudioConverter?
                    var cachedSrcSampleRate: Double = 0
                    var cachedSrcChannels: AVAudioChannelCount = 0
                    let convBuf = AVAudioPCMBuffer(pcmFormat: targetFmt, frameCapacity: 4096)
                    var scaledBuf = [Float](repeating: 0, count: 4096)

                    // Pass format: nil so AVAudioEngine never throws a format-mismatch NSException
                    input.installTap(onBus: 0, bufferSize: 960, format: nil) { [weak self] buffer, _ in
                        guard let self, buffer.frameLength > 0 else { return }
                        let srcFmt = buffer.format
                        let rawPtr: UnsafePointer<Float>
                        let outCount: Int

                        if srcFmt.sampleRate == 48_000.0 && srcFmt.channelCount == 1 && srcFmt.commonFormat == .pcmFormatFloat32,
                           let ch = buffer.floatChannelData?[0] {
                            rawPtr = UnsafePointer(ch)
                            outCount = Int(buffer.frameLength)
                        } else if let convBuf {
                            if cachedConverter == nil || cachedSrcSampleRate != srcFmt.sampleRate || cachedSrcChannels != srcFmt.channelCount {
                                cachedConverter = AVAudioConverter(from: srcFmt, to: targetFmt)
                                cachedSrcSampleRate = srcFmt.sampleRate
                                cachedSrcChannels = srcFmt.channelCount
                            }
                            guard let converter = cachedConverter else { return }
                            let ratio = 48_000.0 / max(1.0, srcFmt.sampleRate)
                            let estFrames = AVAudioFrameCount(min(Double(convBuf.frameCapacity), ceil(Double(buffer.frameLength) * ratio) + 32))
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
                            rawPtr = UnsafePointer(ch)
                            outCount = Int(convBuf.frameLength)
                        } else {
                            return
                        }

                        let gain: Float
                        if self.levelLock.try() {
                            gain = self._micGain
                            self.levelLock.unlock()
                        } else {
                            gain = 1.0
                        }

                        var peak: Float = 0.00001
                        if abs(gain - 1.0) < 0.02 || outCount > scaledBuf.count {
                            for i in 0..<outCount {
                                let a = abs(rawPtr[i])
                                if a > peak { peak = a }
                            }
                            if self.levelLock.try() {
                                self._micPeakDbfs = max(-60.0, 20.0 * log10(peak))
                                self.levelLock.unlock()
                            }
                            self.onMicFloat?(rawPtr, outCount)
                        } else {
                            for i in 0..<outCount {
                                let v = max(-1.0, min(1.0, rawPtr[i] * gain))
                                scaledBuf[i] = v
                                let a = abs(v)
                                if a > peak { peak = a }
                            }
                            if self.levelLock.try() {
                                self._micPeakDbfs = max(-60.0, 20.0 * log10(peak))
                                self.levelLock.unlock()
                            }
                            scaledBuf.withUnsafeBufferPointer { bp in
                                if let base = bp.baseAddress {
                                    self.onMicFloat?(base, outCount)
                                }
                            }
                        }
                    }
                    hasInstalledTap = true
                }
            }

            audioEngine.prepare()
            try audioEngine.start()
            isRunning = true
            return true
        } catch {
            return false
        }
    }

    private func cleanupOnQueue(deactivateSession: Bool = false) {
        if hasInstalledTap {
            audioEngine.inputNode.removeTap(onBus: 0)
            hasInstalledTap = false
        }
        if isRunning {
            audioEngine.stop()
            isRunning = false
        }
        if let node = sourceNode {
            audioEngine.detach(node)
            sourceNode = nil
        }
        audioEngine.reset()
        levelLock.lock()
        _micPeakDbfs = -60.0
        _ifbPeakDbfs = -60.0
        levelLock.unlock()
        if deactivateSession {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }

    private func handleRouteChangeOnQueue(rawReason: UInt?) {
        guard shouldRun, let rawReason, let reason = AVAudioSession.RouteChangeReason(rawValue: rawReason) else { return }
        // ONLY restart on genuine external hardware route changes; ignore .categoryChange / .override loops!
        guard reason == .newDeviceAvailable || reason == .oldDeviceUnavailable else { return }
        routeDebounceWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.shouldRun else { return }
            self.cleanupOnQueue()
            self.startOnQueue()
        }
        routeDebounceWork = work
        audioQueue.asyncAfter(deadline: .now() + 0.30, execute: work)
    }

    private func handleInterruptionOnQueue(rawType: UInt?) {
        guard let rawType, let type = AVAudioSession.InterruptionType(rawValue: rawType) else { return }
        if type == .began {
            cleanupOnQueue()
        } else if type == .ended && shouldRun {
            startOnQueue()
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

    // MARK: - Tally & Duplex Intercom (independent of video streaming!)
    @Published var isIntercomActive = false
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
    @Published var micGain: Float = 1.0 {
        didSet { intercomAudio.micGain = micGain }
    }

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
        didSet { UserDefaults.standard.set(stabilizationLevel.rawValue, forKey: "cam_stabilization_level_v3") }
    }
    @Published var activeHardwareStabilization: StabilizationLevel = .off
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
                    applyLiveEncoderChange(updateCapture: true, forceKeyframe: true)
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
                    applyLiveEncoderChange(updateCapture: true, updateFps: true, forceKeyframe: true)
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
                if isStreaming && !isDemoMode {
                    // Instant zero-disconnect codec switch on the exact same UDP socket & port!
                    applyLiveEncoderChange(updateCapture: false, updateCodec: true, forceKeyframe: true)
                }
            }
        }
    }
    @Published var bitrateMbps: Double = 20.0 {
        didSet {
            guard abs(oldValue - bitrateMbps) >= 0.1 else { return }
            if isStreaming && !isDemoMode {
                let kbps = UInt32(max(2000, Int(bitrateMbps * 1000.0)))
                senderLock.lock()
                if let s = senderHandle {
                    lv_sender_set_bitrate(s, kbps)
                }
                senderLock.unlock()
            }
            if !isApplyingRemoteSync {
                scheduleDebouncedConfigSync()
            }
        }
    }
    @Published var adaptiveQuality: Bool = true {
        didSet {
            guard oldValue != adaptiveQuality else { return }
            if isStreaming && !isDemoMode {
                let on: Int32 = adaptiveQuality ? 1 : 0
                senderLock.lock()
                if let s = senderHandle {
                    lv_sender_set_adaptive_quality(s, on, 35, 95)
                }
                senderLock.unlock()
            }
        }
    }
    @Published var fecEnabled: Bool = true {
        didSet {
            guard oldValue != fecEnabled else { return }
            if isStreaming && !isDemoMode {
                let fec: Int32 = fecEnabled ? 6 : 0
                senderLock.lock()
                if let s = senderHandle {
                    lv_sender_set_fec_group(s, fec)
                }
                senderLock.unlock()
            }
        }
    }
    @Published var colorProfile = "Rec.709"
    private var isApplyingRemoteSync = false
    private var localConfigSyncRev: UInt64 = 0
    private var lastRemoteConfigSyncRev: UInt64 = 0
    private var configSyncDebounceTask: Task<Void, Never>?
    private var initialRegisterTask: Task<Void, Never>?
    private var activeStreamIp: String = ""
    private var activeVideoPort: Int = 0
    private var activeTallyIp: String = ""
    private var activeTallyPort: Int = 0
    private var senderGeneration: UInt64 = 0
    private let senderLifecycleQueue = DispatchQueue(label: "com.manul02.lanvid.camera.sender.lifecycle", qos: .userInitiated)

    // MARK: - iPhone Duo Mode
    @Published var duoMode: DuoMode = .singleHUD
    @Published var duoRole: DuoRole = .camA
    @Published var selectedDuoPeerHost: String?
    @Published var multiCamSupported: Bool = AVCaptureMultiCamSession.isMultiCamSupported

    // MARK: - Telemetry
    @Published var telemetry = CameraTelemetry()

    // MARK: - Discovery & Duo Sync
    let browser = DuoSyncBrowser()

    // MARK: - AVFoundation Sessions (strictly separated sessionQueue & videoOutputQueue!)
    nonisolated let singleSession = AVCaptureSession()
    nonisolated let multiCamSession: AVCaptureMultiCamSession? = AVCaptureMultiCamSession.isMultiCamSupported ? AVCaptureMultiCamSession() : nil
    private let sessionQueue = DispatchQueue(label: "com.manul02.lanvid.camera.session", qos: .userInitiated)
    private let videoOutputQueue = DispatchQueue(label: "com.manul02.lanvid.camera.videoout", qos: .userInteractive)

    // Device state owned on sessionQueue (never lockForConfiguration on Main Thread!)
    private nonisolated(unsafe) var primaryDevice: AVCaptureDevice?
    private nonisolated(unsafe) var secondaryDevice: AVCaptureDevice?
    private nonisolated(unsafe) var isUsingVirtualRearDevice = false
    private nonisolated(unsafe) var wideBaseZoomFactor: CGFloat = 1.0
    private nonisolated(unsafe) var configuredIsFront = false
    private nonisolated(unsafe) var configuredResolution: CineResolution?
    private nonisolated(unsafe) var configuredFps: Int = 0
    private nonisolated(unsafe) var configuredMultiCam = false
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

    // Real AVFoundation capture-to-delegate pipeline lag (exposes EIS lookahead buffer in real time!)
    private let captureLagLock = NSLock()
    private nonisolated(unsafe) var _smoothedCaptureLagMs: Double = 0.0

    private let tallyLock = NSLock()
    private nonisolated(unsafe) var tallyHandle: OpaquePointer?
    private nonisolated(unsafe) var isMicTalkOpen: Bool = false
    private nonisolated(unsafe) var isIfbListenOpen: Bool = true
    private let tallyPollQueue = DispatchQueue(label: "com.manul02.lanvid.camera.tally", qos: .utility)
    private var tallyPollTimer: DispatchSourceTimer?
    private nonisolated(unsafe) var lastPolledSpeaking: Bool = false
    private nonisolated(unsafe) var lastPolledDuck: Float = 1.0

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
        // Default to hardware OIS only (.off = 0 ms lookahead) using fresh v3 key so stale "ultra" (~2 s EIS) resets cleanly!
        let savedStab = StabilizationLevel(rawValue: ud.string(forKey: "cam_stabilization_level_v3") ?? "") ?? .off
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

        // Critical: prevent AVCaptureSession from hijacking AVAudioSession behind IntercomAudioController!
        singleSession.automaticallyConfiguresApplicationAudioSession = false
        multiCamSession?.automaticallyConfiguresApplicationAudioSession = false

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
        sessionObservers.append(nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                if !self.isStreaming {
                    self.sessionQueue.async { [singleSession = self.singleSession, multiCamSession = self.multiCamSession] in
                        if singleSession.isRunning { singleSession.stopRunning() }
                        if let mc = multiCamSession, mc.isRunning { mc.stopRunning() }
                    }
                    self.isPreviewActive = false
                }
            }
        })
        sessionObservers.append(nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                if !self.isDemoMode && !self.isPreviewActive && self.duoMode != .duoCompanion {
                    self.startLocalPreviewIfAuthorized()
                }
            }
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

        intercomAudio.onMicFloat = { [weak self] ptr, count in
            guard let self else { return }
            guard self.tallyLock.try() else { return }
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
        } else if status == .notDetermined {
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                Task { @MainActor in
                    guard let self, granted, !self.isDemoMode else { return }
                    self.configureAndStartCapture()
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
        let wasStreaming = isStreaming && !isDemoMode
        let wasIntercom = isIntercomActive && !isDemoMode
        let videoChanged = (destIp != studio.host || videoPort != Int(studio.videoPort))
        let tallyChanged = (destIp != studio.host || tallyPort != Int(studio.tallyPort))
        destIp = studio.host
        videoPort = Int(studio.videoPort)
        tallyPort = Int(studio.tallyPort)
        statusText = tr("Обрано студію: ", "Selected studio: ") + studio.name
        showTouchToast(studio.name, icon: "desktopcomputer")
        if wasStreaming && (videoChanged || tallyChanged) {
            startLiveStreamingAuthorized()
        } else if wasIntercom && tallyChanged {
            _ = ensureStudioControlLink(ip: studio.host, port: tallyPort, forceReconnect: true)
            statusText = tr("Інтерком → ", "Intercom → ") + "\(studio.host):\(tallyPort)"
        }
    }

    func startDemoCineStudio(tally: ProTallyState = .preview, mode: DuoMode = .singleHUD) {
        stopIntercom(silent: true)
        stopStreaming(keepPreview: false)
        isDemoMode = true
        isStreaming = true
        isIntercomActive = true
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
        stabilizationLevel = .standard
        activeHardwareStabilization = .standard
        supportedStabilizationLevels = StabilizationLevel.allCases

        var t = telemetry
        t.codecBackend = "VideoToolbox H.265 (HW)"
        t.bitrateMbps = 35.2
        t.latencyMs = 7.8
        t.jitterMs = 0.31
        t.lossPct = 0.0
        t.bufferPct = 98
        t.actualFps = 60.0
        telemetry = t

        statusText = tr("Demo Cine Studio активна • Без підключення ПК", "Demo Cine Studio active • No PC required")
        browser.start(localRole: duoRole, localLabel: cameraLabel, demoMode: true)
        startTelemetryTimer()
    }

    func exitDemoMode() {
        isDemoMode = false
        stopIntercom(silent: true)
        stopStreaming(keepPreview: false)
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

    // MARK: - Multi-Level Image Stabilization (runs asynchronously on sessionQueue)

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
            setStabilizationLevel(all[(idx + 1) % all.count])
        }
    }

    private nonisolated static func isHeavyLookaheadMode(_ mode: AVCaptureVideoStabilizationMode) -> Bool {
        if mode == .cinematic || mode == .cinematicExtended { return true }
        if #available(iOS 18.0, *), mode == .cinematicExtendedEnhanced { return true }
        return false
    }

    private nonisolated static func resolveAVStabilizationMode(
        for requested: StabilizationLevel,
        format: AVCaptureDevice.Format?
    ) -> (mode: AVCaptureVideoStabilizationMode, resolvedLevel: StabilizationLevel) {
        // Honest 5-tier latency ladder:
        // .off       -> .off (0 ms, hardware OIS stays active)
        // .standard  -> .lowLatency (iOS 26) / .previewOptimized (iOS 17+) (~20-35 ms, live-safe!)
        // .cinematic -> .standard (~70-100 ms, balanced live-safe EIS)
        // .active    -> .cinematic (~350-600 ms lookahead)
        // .ultra     -> .cinematicExtendedEnhanced / .cinematicExtended (~1.5-2.0 s lookahead!)
        let candidates: [(AVCaptureVideoStabilizationMode, StabilizationLevel)]
        switch requested {
        case .off:
            return (.off, .off)
        case .standard:
            if #available(iOS 26.0, *) {
                candidates = [(.lowLatency, .standard), (.previewOptimized, .standard), (.standard, .cinematic), (.off, .off)]
            } else if #available(iOS 17.0, *) {
                candidates = [(.previewOptimized, .standard), (.standard, .cinematic), (.off, .off)]
            } else {
                candidates = [(.standard, .cinematic), (.off, .off)]
            }
        case .cinematic:
            candidates = [(.standard, .cinematic), (.off, .off)]
        case .active:
            candidates = [(.cinematic, .active), (.standard, .cinematic), (.off, .off)]
        case .ultra:
            if #available(iOS 18.0, *) {
                candidates = [
                    (.cinematicExtendedEnhanced, .ultra),
                    (.cinematicExtended, .ultra),
                    (.cinematic, .active),
                    (.standard, .cinematic),
                    (.off, .off)
                ]
            } else {
                candidates = [
                    (.cinematicExtended, .ultra),
                    (.cinematic, .active),
                    (.standard, .cinematic),
                    (.off, .off)
                ]
            }
        }
        guard let fmt = format else { return candidates.first ?? (.off, .off) }
        for (mode, lvl) in candidates {
            if mode == .off || fmt.isVideoStabilizationModeSupported(mode) {
                return (mode, lvl)
            }
        }
        return (.off, .off)
    }

    private nonisolated static func querySupportedStabilizationLevels(format: AVCaptureDevice.Format) -> [StabilizationLevel] {
        var result: [StabilizationLevel] = [.off]
        var lowLatSupported = false
        if #available(iOS 17.0, *), format.isVideoStabilizationModeSupported(.previewOptimized) {
            lowLatSupported = true
        }
        if #available(iOS 26.0, *), format.isVideoStabilizationModeSupported(.lowLatency) {
            lowLatSupported = true
        }
        if lowLatSupported { result.append(.standard) }
        if format.isVideoStabilizationModeSupported(.standard) { result.append(.cinematic) }
        if format.isVideoStabilizationModeSupported(.cinematic) { result.append(.active) }
        var ultraSupported = format.isVideoStabilizationModeSupported(.cinematicExtended)
        if #available(iOS 18.0, *), format.isVideoStabilizationModeSupported(.cinematicExtendedEnhanced) {
            ultraSupported = true
        }
        if ultraSupported { result.append(.ultra) }
        return result
    }

    private func applyStabilizationToActiveSessions(_ level: StabilizationLevel) {
        sessionQueue.async { [weak self] in
            guard let self else { return }
            let dev = self.primaryDevice
            let resolved = Self.resolveAVStabilizationMode(for: level, format: dev?.activeFormat)
            var appliedLevel = resolved.resolvedLevel

            // If leaving a heavy temporal lookahead EIS mode (0.5–2.0 s buffer), briefly cycle the
            // capture session so Apple's ISP immediately flushes its buffered frames (UDP sender stays open!).
            var leavingHeavySingle = false
            for out in self.singleSession.outputs {
                if let conn = out.connection(with: .video),
                   Self.isHeavyLookaheadMode(conn.activeVideoStabilizationMode),
                   !Self.isHeavyLookaheadMode(resolved.mode) {
                    leavingHeavySingle = true
                }
            }
            let wasRunningSingle = self.singleSession.isRunning
            if leavingHeavySingle && wasRunningSingle {
                self.singleSession.stopRunning()
            }
            self.singleSession.beginConfiguration()
            for out in self.singleSession.outputs {
                if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                    conn.preferredVideoStabilizationMode = resolved.mode
                }
            }
            self.singleSession.commitConfiguration()
            if leavingHeavySingle && wasRunningSingle {
                self.singleSession.startRunning()
            }
            for out in self.singleSession.outputs {
                if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                    appliedLevel = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolved.resolvedLevel)
                }
            }

            if let mc = self.multiCamSession {
                var leavingHeavyMulti = false
                for out in mc.outputs {
                    if let conn = out.connection(with: .video),
                       Self.isHeavyLookaheadMode(conn.activeVideoStabilizationMode),
                       !Self.isHeavyLookaheadMode(resolved.mode) {
                        leavingHeavyMulti = true
                    }
                }
                let wasRunningMulti = mc.isRunning
                if leavingHeavyMulti && wasRunningMulti { mc.stopRunning() }
                mc.beginConfiguration()
                for out in mc.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                        conn.preferredVideoStabilizationMode = resolved.mode
                    }
                }
                mc.commitConfiguration()
                if leavingHeavyMulti && wasRunningMulti { mc.startRunning() }
                for out in mc.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                        appliedLevel = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolved.resolvedLevel)
                    }
                }
            }

            self.captureLagLock.lock()
            self._smoothedCaptureLagMs = 0.0
            self.captureLagLock.unlock()

            let supported = dev.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
            Task { @MainActor [weak self] in
                self?.activeHardwareStabilization = appliedLevel
                self?.supportedStabilizationLevels = supported
            }
        }
    }

    private nonisolated static func level(from mode: AVCaptureVideoStabilizationMode, fallback: StabilizationLevel) -> StabilizationLevel {
        switch mode {
        case .off: return .off
        case .standard: return .cinematic
        case .cinematic: return .active
        case .cinematicExtended: return .ultra
        case .auto: return fallback
        default:
            if #available(iOS 26.0, *), mode == .lowLatency {
                return .standard
            }
            if #available(iOS 17.0, *), mode == .previewOptimized {
                return .standard
            }
            if #available(iOS 18.0, *), mode == .cinematicExtendedEnhanced {
                return .ultra
            }
            return fallback
        }
    }

    // MARK: - Intuitive Viewfinder Touch Interactions (all hardware locks off Main Thread!)

    func showTouchToast(_ text: String, icon: String = "viewfinder") {
        if touchHUD.toastText != text || touchHUD.toastIcon != icon {
            touchHUD.toastText = text
            touchHUD.toastIcon = icon
        }
        toastHideTask?.cancel()
        toastHideTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.22)) {
                self?.touchHUD.toastText = nil
            }
        }
    }

    private func scheduleReticleAutoHide(after seconds: Double = 2.6) {
        reticleHideTask?.cancel()
        reticleHideTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            guard !Task.isCancelled, let self else { return }
            if !self.aeAfLocked && !self.touchHUD.adjustingExposure && !self.touchHUD.adjustingFocus {
                withAnimation(.easeOut(duration: 0.25)) {
                    self.touchHUD.visible = false
                }
            }
        }
    }

    func focusAndExpose(atDevicePoint devicePoint: CGPoint, viewNormalizedPoint: CGPoint) {
        lightImpact.impactOccurred()
        aeAfLocked = false
        autoFocus = true
        autoExposure = true
        exposureBias = 0.0

        var hud = touchHUD
        hud.normalizedPoint = viewNormalizedPoint
        hud.visible = true
        hud.locked = false
        hud.adjustingExposure = false
        hud.adjustingFocus = false
        withAnimation(.spring(response: 0.22, dampingFraction: 0.78)) {
            touchHUD = hud
        }
        scheduleReticleAutoHide(after: 2.8)

        if isDemoMode {
            let distFromCenter = hypot(viewNormalizedPoint.x - 0.5, viewNormalizedPoint.y - 0.5)
            focusPosition = Float(max(0.15, min(0.85, 0.48 + (distFromCenter - 0.15) * 0.25)))
            showTouchToast(tr("Фокус і експозиція", "Focus & Exposure"), icon: "viewfinder")
            return
        }

        sessionQueue.async { [weak self] in
            guard let dev = self?.primaryDevice else { return }
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
    }

    func lockFocusAndExposure(atDevicePoint devicePoint: CGPoint, viewNormalizedPoint: CGPoint) {
        mediumImpact.impactOccurred()
        aeAfLocked = true
        autoFocus = false
        autoExposure = false

        var hud = touchHUD
        hud.normalizedPoint = viewNormalizedPoint
        hud.visible = true
        hud.locked = true
        hud.adjustingExposure = false
        hud.adjustingFocus = false
        withAnimation(.spring(response: 0.22, dampingFraction: 0.72)) {
            touchHUD = hud
        }
        reticleHideTask?.cancel()
        showTouchToast(tr("AE / AF заблоковано", "AE / AF Locked"), icon: "lock.fill")

        if isDemoMode { return }

        sessionQueue.async { [weak self] in
            guard let self, let dev = self.primaryDevice else { return }
            var lockedLensPos: Float?
            do {
                try dev.lockForConfiguration()
                if dev.isFocusPointOfInterestSupported { dev.focusPointOfInterest = devicePoint }
                if dev.isExposurePointOfInterestSupported { dev.exposurePointOfInterest = devicePoint }
                if dev.isFocusModeSupported(.locked) {
                    dev.focusMode = .locked
                    lockedLensPos = dev.lensPosition
                }
                if dev.isExposureModeSupported(.locked) {
                    dev.exposureMode = .locked
                }
                dev.unlockForConfiguration()
            } catch {}
            if let lp = lockedLensPos {
                Task { @MainActor [weak self] in self?.focusPosition = lp }
            }
        }
    }

    func unlockAeAf() {
        guard aeAfLocked else { return }
        resetTouchFocusAndExposure()
    }

    func setExposureBias(_ newBias: Float, activeDrag: Bool = false) {
        let clamped = max(-3.0, min(3.0, newBias))
        let nearZero = abs(clamped) < 0.08
        if nearZero && !lastEvHapticZeroCross {
            selectionFeedback.selectionChanged()
        }
        lastEvHapticZeroCross = nearZero

        let snapped = nearZero ? 0.0 : (round(clamped * 10.0) / 10.0)
        if abs(exposureBias - snapped) >= 0.04 || touchHUD.adjustingExposure != activeDrag {
            exposureBias = snapped
            var hud = touchHUD
            hud.visible = true
            hud.adjustingExposure = activeDrag
            touchHUD = hud
        }

        if !activeDrag {
            scheduleReticleAutoHide(after: 2.2)
        } else {
            reticleHideTask?.cancel()
        }

        let sign = snapped >= 0 ? "+" : ""
        showTouchToast(String(format: "%@ %@%.1f EV", tr("Експозиція", "Exposure"), sign, snapped), icon: "sun.max.fill")

        if isDemoMode { return }

        sessionQueue.async { [weak self] in
            guard let dev = self?.primaryDevice else { return }
            do {
                try dev.lockForConfiguration()
                if dev.exposureMode == .custom && dev.isExposureModeSupported(.continuousAutoExposure) {
                    dev.exposureMode = .continuousAutoExposure
                    Task { @MainActor [weak self] in self?.autoExposure = true }
                }
                let hwClamped = max(dev.minExposureTargetBias, min(dev.maxExposureTargetBias, snapped))
                dev.setExposureTargetBias(hwClamped, completionHandler: nil)
                dev.unlockForConfiguration()
            } catch {}
        }
    }

    func setTouchManualFocus(_ position: Float, activeDrag: Bool = false) {
        let clamped = max(0.0, min(1.0, position))
        let quantized = round(clamped * 100.0) / 100.0
        autoFocus = false
        if abs(focusPosition - quantized) >= 0.005 || touchHUD.adjustingFocus != activeDrag {
            focusPosition = quantized
            touchHUD.adjustingFocus = activeDrag
        }
        if !activeDrag {
            scheduleReticleAutoHide(after: 1.8)
        } else {
            reticleHideTask?.cancel()
        }
        showTouchToast(String(format: "%@ %d%%", tr("Фокус MF", "Manual Focus"), Int(quantized * 100)), icon: "scope")

        if isDemoMode { return }

        sessionQueue.async { [weak self] in
            guard let dev = self?.primaryDevice else { return }
            do {
                try dev.lockForConfiguration()
                if dev.isLockingFocusWithCustomLensPositionSupported {
                    dev.setFocusModeLocked(lensPosition: quantized, completionHandler: nil)
                } else if dev.isFocusModeSupported(.locked) {
                    // Virtual multi-lens cameras (e.g. iPhone 13 mini DualWide) don't support custom lensPosition
                    dev.focusMode = .locked
                }
                dev.unlockForConfiguration()
            } catch {}
        }
    }

    func resetTouchFocusAndExposure() {
        mediumImpact.impactOccurred()
        aeAfLocked = false
        autoFocus = true
        autoExposure = true
        exposureBias = 0.0

        var hud = touchHUD
        hud.locked = false
        hud.normalizedPoint = CGPoint(x: 0.5, y: 0.5)
        hud.visible = true
        hud.adjustingExposure = false
        hud.adjustingFocus = false
        withAnimation(.easeOut(duration: 0.18)) {
            touchHUD = hud
        }
        scheduleReticleAutoHide(after: 1.3)
        showTouchToast(tr("AF / AE Авто · 0.0 EV", "AF / AE Auto · 0.0 EV"), icon: "arrow.counterclockwise")

        if isDemoMode {
            focusPosition = 0.48
            return
        }

        sessionQueue.async { [weak self] in
            guard let dev = self?.primaryDevice else { return }
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
    }

    func setDisplayZoom(_ requestedDisplayZoom: CGFloat, animated: Bool = false, showOverlay: Bool = true) {
        let clampedDisplay = max(minDisplayZoom, min(maxDisplayZoom, requestedDisplayZoom))
        if abs(zoomFactor - clampedDisplay) < 0.01 && !animated { return }
        zoomFactor = clampedDisplay

        let quantized = Int(round(clampedDisplay * 10))
        if [5, 10, 20, 30, 50].contains(quantized) && quantized != lastZoomDetent {
            selectionFeedback.selectionChanged()
        }
        lastZoomDetent = quantized

        let currentLens = lens
        if currentLens != .front {
            let nextLens: CineLens
            if clampedDisplay < 0.85 { nextLens = .ultraWide }
            else if clampedDisplay < 1.75 { nextLens = .wide }
            else if clampedDisplay < 2.65 { nextLens = .tele }
            else { nextLens = .tele3x }
            if lens != nextLens { lens = nextLens }
        }

        if showOverlay {
            showTouchToast(String(format: "%@ %.1f×", tr("Зум", "Zoom"), clampedDisplay), icon: "magnifyingglass")
        }

        if isDemoMode { return }

        let maxDisp = maxDisplayZoom
        let activeLensTarget = lens.targetDisplayZoom
        sessionQueue.async { [weak self] in
            guard let self, let dev = self.primaryDevice else { return }
            let hwTarget: CGFloat
            if self.isUsingVirtualRearDevice {
                hwTarget = clampedDisplay * self.wideBaseZoomFactor
            } else {
                hwTarget = max(1.0, clampedDisplay / max(0.5, activeLensTarget))
            }
            let maxHw = min(dev.activeFormat.videoMaxZoomFactor, maxDisp * self.wideBaseZoomFactor)
            let finalHw = max(dev.minAvailableVideoZoomFactor, min(maxHw, hwTarget))
            do {
                try dev.lockForConfiguration()
                if animated {
                    dev.ramp(toVideoZoomFactor: finalHw, withRate: 12.0)
                } else {
                    dev.cancelVideoZoomRamp()
                    dev.videoZoomFactor = finalHw
                }
                dev.unlockForConfiguration()
            } catch {}
        }
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
            stopStreaming(keepPreview: true)
        } else {
            startLiveStreaming()
        }
    }

    // MARK: - Shared Studio Control Link (:5022 Tally / ConfigSync / Intercom)

    @discardableResult
    private func ensureStudioControlLink(ip: String, port: Int, forceReconnect: Bool = false) -> Bool {
        let cleanIp = ip.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanIp.isEmpty else { return false }

        tallyLock.lock()
        let existing = tallyHandle
        tallyLock.unlock()

        if existing != nil && !forceReconnect && activeTallyIp == cleanIp && activeTallyPort == port {
            syncIntercomState()
            if tallyPollTimer == nil {
                startTallyPollTimer()
            }
            sendLocalConfigSyncToStudio()
            return true
        }

        // Tear down stale handle before reconnecting to a new IP/port
        tallyPollTimer?.cancel()
        tallyPollTimer = nil
        tallyLock.lock()
        let oldTally = tallyHandle
        tallyHandle = nil
        tallyLock.unlock()
        if let old = oldTally {
            lv_tally_intercom_destroy(old)
        }

        let th = cleanIp.withCString { ipC in
            cameraLabel.withCString { lblC in
                lv_tally_intercom_create_camera(ipC, UInt16(clamping: port), cameraId, lblC)
            }
        }
        guard let th else {
            activeTallyIp = ""
            activeTallyPort = 0
            return false
        }

        let ctx = Unmanaged.passUnretained(self).toOpaque()
        lv_camera_set_tally_callback(th, { statePtr, userData in
            guard let statePtr, let userData else { return }
            let st = statePtr.pointee
            let engine = Unmanaged<CameraEngine>.fromOpaque(userData).takeUnretainedValue()
            let nextTally = ProTallyState(rawValue: Int(st.tally_state)) ?? .standby
            let nextRec = (st.flags & 0x01) != 0
            Task { @MainActor in
                if engine.tallyState != nextTally { engine.tallyState = nextTally }
                if engine.remoteRec != nextRec { engine.remoteRec = nextRec }
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
        activeTallyIp = cleanIp
        activeTallyPort = port

        syncIntercomState()
        startTallyPollTimer()

        // Immediately register camera_id & video profile with Studio / LanVid Monitor so Tally & Intercom bind instantly!
        sendLocalConfigSyncToStudio()
        initialRegisterTask?.cancel()
        initialRegisterTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 450_000_000)
            guard !Task.isCancelled, let self, (self.isStreaming || self.isIntercomActive), !self.isDemoMode else { return }
            self.sendLocalConfigSyncToStudio()
        }
        return true
    }

    private func teardownStudioControlLinkIfUnused() {
        guard !isStreaming && !isIntercomActive else { return }
        configSyncDebounceTask?.cancel()
        configSyncDebounceTask = nil
        initialRegisterTask?.cancel()
        initialRegisterTask = nil
        lastRemoteConfigSyncRev = 0
        tallyPollTimer?.cancel()
        tallyPollTimer = nil

        tallyLock.lock()
        let oldTally = tallyHandle
        tallyHandle = nil
        isMicTalkOpen = false
        isIfbListenOpen = false
        tallyLock.unlock()
        if let t = oldTally {
            lv_tally_intercom_destroy(t)
        }
        activeTallyIp = ""
        activeTallyPort = 0

        if !isDemoMode {
            tallyState = .standby
            remoteRec = false
            directorSpeaking = false
            duckingGain = 1.0
        }
    }

    private func updateIdleTimerAndTelemetry() {
        let keepAlive = isStreaming || isIntercomActive || isDemoMode
        UIApplication.shared.isIdleTimerDisabled = keepAlive
        if keepAlive {
            if tickTimer == nil {
                startTelemetryTimer()
            }
        } else {
            tickTimer?.invalidate()
            tickTimer = nil
        }
    }

    private func resolveCleanStudioIp() -> String {
        var cleanIp = destIp.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleanIp.isEmpty, let discovered = browser.studios.first {
            cleanIp = discovered.host
            destIp = discovered.host
            videoPort = Int(discovered.videoPort)
            tallyPort = Int(discovered.tallyPort)
        } else if cleanIp != destIp {
            destIp = cleanIp
        }
        return cleanIp
    }

    // MARK: - Independent Duplex Intercom Lifecycle (works WITH or WITHOUT video stream!)

    func toggleIntercom() {
        selectionFeedback.selectionChanged()
        if isDemoMode {
            if isIntercomActive {
                isIntercomActive = false
                if latchActive { latchActive = false }
                if pttActive { pttActive = false }
                showTouchToast(tr("Інтерком вимкнено", "Intercom OFF"), icon: "speaker.slash")
            } else {
                isIntercomActive = true
                showTouchToast(tr("Інтерком увімкнено", "Intercom ON"), icon: "headphones")
            }
            return
        }
        if isIntercomActive {
            stopIntercom()
        } else {
            startIntercom()
        }
    }

    func startIntercom(activatePtt: Bool = false, activateLatch: Bool = false, forceIfbOn: Bool = false) {
        if isDemoMode {
            isIntercomActive = true
            if forceIfbOn { ifbEnabled = true }
            if activateLatch { latchActive = true }
            else if activatePtt { pttActive = true }
            showTouchToast(tr("Інтерком увімкнено", "Intercom ON"), icon: "headphones")
            return
        }

        let aStatus = AVCaptureDevice.authorizationStatus(for: .audio)
        if aStatus == .notDetermined {
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] _ in
                Task { @MainActor in
                    self?.startIntercomAuthorized(activatePtt: activatePtt, activateLatch: activateLatch, forceIfbOn: forceIfbOn)
                }
            }
            return
        }
        startIntercomAuthorized(activatePtt: activatePtt, activateLatch: activateLatch, forceIfbOn: forceIfbOn)
    }

    private func startIntercomAuthorized(activatePtt: Bool = false, activateLatch: Bool = false, forceIfbOn: Bool = false) {
        let cleanIp = resolveCleanStudioIp()
        guard !cleanIp.isEmpty else {
            statusText = tr("Вкажіть IP-адресу LanVid Studio", "Enter LanVid Studio IP address")
            showTouchToast(tr("Вкажіть IP студії", "Enter Studio IP"), icon: "network")
            return
        }

        isIntercomActive = true
        if forceIfbOn { ifbEnabled = true }
        if activateLatch {
            latchActive = true
        } else if activatePtt {
            pttActive = true
        }

        let ok = ensureStudioControlLink(ip: cleanIp, port: tallyPort)
        if !ok {
            isIntercomActive = false
            if latchActive { latchActive = false }
            if pttActive { pttActive = false }
            statusText = tr("Помилка запуску інтеркому", "Failed to start intercom")
            return
        }

        intercomAudio.start()
        syncIntercomState()
        updateIdleTimerAndTelemetry()

        if !isStreaming {
            streamStartWallTime = CACurrentMediaTime()
            statusText = tr("Інтерком → ", "Intercom → ") + "\(cleanIp):\(tallyPort)"
        }
        let micDenied = AVCaptureDevice.authorizationStatus(for: .audio) == .denied
            || AVCaptureDevice.authorizationStatus(for: .audio) == .restricted
        showTouchToast(
            micDenied
                ? tr("Інтерком: лише прослуховування", "Intercom: Listen Only (No Mic)")
                : tr("Інтерком увімкнено", "Intercom ON"),
            icon: "headphones"
        )
    }

    func stopIntercom(silent: Bool = false) {
        let wasActive = isIntercomActive
        isIntercomActive = false
        if latchActive { latchActive = false }
        if pttActive { pttActive = false }
        intercomAudio.stop()
        syncIntercomState()
        teardownStudioControlLinkIfUnused()
        updateIdleTimerAndTelemetry()

        if !isStreaming && !isDemoMode {
            statusText = tr("Готово • Інтерком вимкнено", "Ready • Intercom off")
        }
        if wasActive && !silent {
            showTouchToast(tr("Інтерком вимкнено", "Intercom OFF"), icon: "speaker.slash")
        }
    }

    func togglePtt() {
        lightImpact.impactOccurred()
        if !isIntercomActive {
            startIntercom(activatePtt: true)
            return
        }
        if latchActive {
            latchActive = false
        } else {
            pttActive.toggle()
        }
    }

    func toggleLatch() {
        lightImpact.impactOccurred()
        if !isIntercomActive {
            startIntercom(activateLatch: true)
            return
        }
        latchActive.toggle()
    }

    func toggleIfb() {
        lightImpact.impactOccurred()
        if !isIntercomActive {
            startIntercom(forceIfbOn: true)
            return
        }
        ifbEnabled.toggle()
    }

    // MARK: - Live Video Streaming (independent of Intercom!)

    func startLiveStreaming() {
        if duoMode != .duoCompanion {
            let vStatus = AVCaptureDevice.authorizationStatus(for: .video)
            if vStatus == .notDetermined {
                AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                    Task { @MainActor in
                        guard let self else { return }
                        if granted {
                            self.startLiveStreaming()
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
        startLiveStreamingAuthorized()
    }

    private func stopVideoSenderOnly() {
        senderGeneration &+= 1
        senderLock.lock()
        let oldSender = senderHandle
        senderHandle = nil
        senderLock.unlock()
        if let s = oldSender {
            senderLifecycleQueue.async {
                lv_sender_stop(s)
                lv_sender_destroy(s)
            }
        }
    }

    private func startLiveStreamingAuthorized() {
        stopVideoSenderOnly()
        isDemoMode = false
        lastRemoteConfigSyncRev = 0
        browser.pauseActiveSweep()

        let cleanIp = resolveCleanStudioIp()
        guard !cleanIp.isEmpty else {
            statusText = tr("Вкажіть IP-адресу LanVid Studio", "Enter LanVid Studio IP address")
            return
        }

        if let s = lv_sender_create() {
            var cfg = LV_SenderConfig()
            lv_sdk_sender_config_default(&cfg)
            let ipBytes = Array(cleanIp.utf8.prefix(63))
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

        if duoMode != .duoCompanion {
            configureAndStartCapture(reuseIfMatching: true)
        }
        activeStreamIp = cleanIp
        activeVideoPort = videoPort
        streamStartWallTime = CACurrentMediaTime()
        isStreaming = true
        // Reuse existing :5022 control socket if Intercom already opened it to the same host, or open for Tally/Sync!
        _ = ensureStudioControlLink(ip: cleanIp, port: tallyPort)
        statusText = "\(resolution.rawValue) \(fps)p \(codec.shortName) → \(cleanIp):\(videoPort)"
        updateIdleTimerAndTelemetry()
    }

    func reconnectIfEndpointChanged() {
        guard !isDemoMode else { return }
        let cleanIp = destIp.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleanIp != destIp { destIp = cleanIp }
        guard !cleanIp.isEmpty else { return }

        let videoChanged = (cleanIp != activeStreamIp || videoPort != activeVideoPort)
        let tallyChanged = (cleanIp != activeTallyIp || tallyPort != activeTallyPort)

        if isStreaming && (videoChanged || tallyChanged) {
            startLiveStreamingAuthorized()
        } else if isIntercomActive && tallyChanged {
            _ = ensureStudioControlLink(ip: cleanIp, port: tallyPort, forceReconnect: true)
            statusText = tr("Інтерком → ", "Intercom → ") + "\(cleanIp):\(tallyPort)"
        }
    }

    func stopStreaming(keepPreview: Bool = true) {
        stopVideoSenderOnly()
        if !keepPreview {
            isPreviewActive = false
            sessionQueue.async { [weak self] in
                guard let self else { return }
                if self.singleSession.isRunning { self.singleSession.stopRunning() }
                if let mc = self.multiCamSession, mc.isRunning { mc.stopRunning() }
            }
        }

        isStreaming = false
        teardownStudioControlLinkIfUnused()
        updateIdleTimerAndTelemetry()

        if !isDemoMode {
            if isIntercomActive {
                statusText = tr("Інтерком → ", "Intercom → ") + "\(activeTallyIp):\(activeTallyPort)"
            } else {
                statusText = tr("Ефір зупинено", "Stream stopped")
            }
        }
    }

    private func startTallyPollTimer() {
        tallyPollTimer?.cancel()
        lastPolledSpeaking = directorSpeaking
        lastPolledDuck = duckingGain
        let timer = DispatchSource.makeTimerSource(queue: tallyPollQueue)
        // Poll at 100 Hz (10 ms) to match 10 ms Opus intercom voice frames cleanly without underruns
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

            // Only hop to MainActor when state ACTUALLY changes (avoids 50-100 Hz idle Task spam!)
            let speakingChanged = (speaking != self.lastPolledSpeaking)
            let duckChanged = (abs(duck - self.lastPolledDuck) > 0.02)
            if speakingChanged || duckChanged {
                self.lastPolledSpeaking = speaking
                self.lastPolledDuck = duck
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    if speakingChanged { self.directorSpeaking = speaking }
                    if duckChanged { self.duckingGain = duck }
                }
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
                sessionQueue.async { [weak self] in
                    guard let self else { return }
                    if self.singleSession.isRunning { self.singleSession.stopRunning() }
                    if let mc = self.multiCamSession, mc.isRunning { mc.stopRunning() }
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
        let snapAutoExp = autoExposure
        let snapShutter = shutterAngle
        let snapIso = isoValue
        let snapExpBias = exposureBias
        let snapAutoWB = autoWhiteBalance
        let snapKelvin = wbKelvin
        let snapTint = wbTint
        let snapAutoFoc = autoFocus
        let snapFocusPos = focusPosition
        let snapFps = fps
        let snapSecAuto = secondaryAutoExposure
        let snapSecBias = secondaryExposureBias

        sessionQueue.async { [weak self] in
            guard let self else { return }
            if let dev = self.primaryDevice {
                Self.applyExposureAndFocus(
                    to: dev,
                    fps: snapFps,
                    autoExp: snapAutoExp,
                    shutter: snapShutter,
                    iso: snapIso,
                    exposureBias: snapExpBias,
                    autoWB: snapAutoWB,
                    kelvin: snapKelvin,
                    tint: snapTint,
                    autoFoc: snapAutoFoc,
                    focusPos: snapFocusPos
                )
            }
            if let sec = self.secondaryDevice {
                do {
                    try sec.lockForConfiguration()
                    if snapSecAuto {
                        if sec.isExposureModeSupported(.continuousAutoExposure) {
                            sec.exposureMode = .continuousAutoExposure
                        }
                        let clampedBias = max(sec.minExposureTargetBias, min(sec.maxExposureTargetBias, snapSecBias))
                        sec.setExposureTargetBias(clampedBias, completionHandler: nil)
                    }
                    sec.unlockForConfiguration()
                } catch {}
            }
        }
        if duoMode == .duoCompanion {
            sendCompanionCommandToSelectedPeer()
        }
    }

    private nonisolated static func applyExposureAndFocus(
        to dev: AVCaptureDevice,
        fps: Int,
        autoExp: Bool,
        shutter: Int,
        iso: Float,
        exposureBias: Float,
        autoWB: Bool,
        kelvin: Float,
        tint: Float,
        autoFoc: Bool,
        focusPos: Float
    ) {
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
                var maxDur = dev.activeFormat.maxExposureDuration
                let maxFrameDur = dev.activeVideoMaxFrameDuration
                if CMTIME_IS_NUMERIC(maxFrameDur) && CMTimeCompare(maxFrameDur, .zero) > 0 {
                    maxDur = CMTimeMinimum(maxDur, maxFrameDur)
                }
                let clampedDur = CMTimeMinimum(maxDur, CMTimeMaximum(minDur, rawDur))
                let clampedIso = max(dev.activeFormat.minISO, min(dev.activeFormat.maxISO, iso))
                dev.setExposureModeCustom(duration: clampedDur, iso: clampedIso)
            }
            if autoWB {
                if dev.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) { dev.whiteBalanceMode = .continuousAutoWhiteBalance }
            } else if dev.isLockingWhiteBalanceWithCustomDeviceGainsSupported {
                let tt = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(temperature: kelvin, tint: tint)
                let gains = dev.deviceWhiteBalanceGains(for: tt)
                let clamped = AVCaptureDevice.WhiteBalanceGains(
                    redGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.redGain)),
                    greenGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.greenGain)),
                    blueGain: max(1.0, min(dev.maxWhiteBalanceGain, gains.blueGain))
                )
                dev.setWhiteBalanceModeLocked(with: clamped)
            } else if dev.isWhiteBalanceModeSupported(.locked) {
                dev.whiteBalanceMode = .locked
            }
            if autoFoc {
                if dev.isFocusModeSupported(.continuousAutoFocus) { dev.focusMode = .continuousAutoFocus }
            } else if dev.isLockingFocusWithCustomLensPositionSupported {
                dev.setFocusModeLocked(lensPosition: max(0.0, min(1.0, focusPos)))
            } else if dev.isFocusModeSupported(.locked) {
                dev.focusMode = .locked
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
        if targetLens != lens { selectLens(targetLens) }

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
                applyLiveEncoderChange(updateCapture: true, forceKeyframe: true)
            }
        }

        autoFocus = (ctrl.focus_auto != 0)
        if !autoFocus {
            focusPosition = Float(min(1000, ctrl.focus_pos)) / 1000.0
        }
        applyManualCameraControls()

        if ctrl.zoom_x100 >= 100 {
            setDisplayZoom(CGFloat(ctrl.zoom_x100) / 100.0, animated: true, showOverlay: true)
        }
    }

    private func applyRemoteVideoConfigSync(_ cfg: LV_VideoConfigSync) {
        // Pure IDR keyframe request (kVideoConfigFlagRequestIdr = 0x01):
        // Trigger an immediate hardware IDR and RETURN so we NEVER overwrite local camera/encoder settings!
        if (cfg.flags & 0x01) != 0 {
            senderLock.lock()
            let s = senderHandle
            if let s { lv_sender_force_keyframe(s) }
            senderLock.unlock()
            return
        }

        // Echo guard: ignore camera-originated packets
        if (cfg.flags & 0x02) != 0 { return }

        // Deduplicate 3x UDP redundancy & out-of-order stale config packets
        if cfg.config_revision != 0 {
            if cfg.config_revision <= lastRemoteConfigSyncRev { return }
            lastRemoteConfigSyncRev = cfg.config_revision
        }

        isApplyingRemoteSync = true
        defer { isApplyingRemoteSync = false }

        if cfg.assigned_cam_number > 0 { assignedCamNumber = Int(cfg.assigned_cam_number) }
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

        if cfg.video_delay_ms >= 5 { syncedDelayMs = Int(cfg.video_delay_ms) }
        syncedFixedDelay = (cfg.fixed_delay_mode != 0)
        configSynced = true

        // Only apply video encoder/lens fields when Studio sent a full config (target_fps >= 15 && bitrate_kbps >= 1000)
        guard cfg.target_fps >= 15 && cfg.bitrate_kbps >= 1000 else { return }

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
        let targetFps = Int(cfg.target_fps)
        let targetMbps = min(100.0, max(5.0, Double(cfg.bitrate_kbps) / 1000.0))

        let needCaptureRestart = (targetRes != resolution || targetFps != fps)
        let needCodecUpdate = (targetCodec != codec)
        let needFpsUpdate = (targetFps != fps)
        let needLensSwitch = (targetLens != lens)

        resolution = targetRes
        codec = targetCodec
        fps = targetFps
        bitrateMbps = targetMbps

        if needLensSwitch { selectLens(targetLens) }
        if isStreaming && !isDemoMode {
            let kbps = UInt32(max(2000, Int(bitrateMbps * 1000.0)))
            senderLock.lock()
            if let s = senderHandle { lv_sender_set_bitrate(s, kbps) }
            senderLock.unlock()
            if needCaptureRestart || needCodecUpdate || needFpsUpdate {
                applyLiveEncoderChange(
                    updateCapture: needCaptureRestart,
                    updateCodec: needCodecUpdate,
                    updateFps: needFpsUpdate,
                    forceKeyframe: true
                )
            }
        }
    }

    private func scheduleDebouncedConfigSync() {
        configSyncDebounceTask?.cancel()
        configSyncDebounceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            self?.sendLocalConfigSyncToStudio()
        }
    }

    /// Zero-disconnect live settings switch: keeps the EXACT SAME UDP socket, local port, frame_id sequence,
    /// OBS/Monitor slot binding, clock sync, and Tally association — NEVER drops or migrates the stream!
    private func applyLiveEncoderChange(
        updateCapture: Bool,
        updateCodec: Bool = false,
        updateFps: Bool = false,
        forceKeyframe: Bool = true
    ) {
        let snapCodec = codec.lvCodecValue
        let snapFps = Double(fps)
        senderLock.lock()
        if let s = senderHandle {
            if updateCodec { lv_sender_set_codec(s, snapCodec) }
            if updateFps { lv_sender_set_fps(s, snapFps) }
            if forceKeyframe { lv_sender_force_keyframe(s) }
        }
        senderLock.unlock()

        if updateCapture && duoMode != .duoCompanion {
            configureAndStartCapture(reuseIfMatching: true)
        }
        statusText = "\(resolution.rawValue) \(fps)p \(codec.shortName) → \(destIp):\(videoPort)"
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
        let talkOpen = isIntercomActive && (pttActive || latchActive)
        let listenOpen = isIntercomActive && ifbEnabled
        let vol: Float = listenOpen ? ifbVolume : 0.0
        tallyLock.lock()
        isMicTalkOpen = talkOpen
        isIfbListenOpen = listenOpen
        if let th = tallyHandle {
            lv_intercom_set_state(th, talkOpen ? 1 : 0, listenOpen ? 0 : 1, vol, 0.0)
        }
        tallyLock.unlock()
        if isIntercomActive && !isDemoMode {
            intercomAudio.ensureStarted()
        }
    }

    // MARK: - AVFoundation Capture Setup (Dedicated sessionQueue + videoOutputQueue)

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
        sessionQueue.async { [weak self] in
            guard let self else { return }
            for out in self.singleSession.outputs {
                if let conn = out.connection(with: .video), conn.isVideoOrientationSupported, conn.videoOrientation != orient {
                    conn.videoOrientation = orient
                }
            }
            if let mc = self.multiCamSession {
                for out in mc.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoOrientationSupported, conn.videoOrientation != orient {
                        conn.videoOrientation = orient
                    }
                }
            }
        }
    }

    /// Picks an exact-resolution 8-bit `420v` AVCaptureDevice.Format that genuinely supports `targetFps` (e.g. 60 fps)
    /// without falling into Apple's 30 fps sessionPreset trap or slow-mo 240 fps narrow-FOV formats.
    private nonisolated static func selectOptimalFormat(
        for dev: AVCaptureDevice,
        targetRes: CineResolution,
        targetFps: Int,
        targetStab: StabilizationLevel
    ) -> (format: AVCaptureDevice.Format, effectiveFps: Int)? {
        let reqW = targetRes.width
        let reqH = targetRes.height
        let desiredFps = Double(max(15, targetFps))

        func scoreFormat(_ fmt: AVCaptureDevice.Format) -> (tier: Int, score: Int, maxFps: Double)? {
            let desc = fmt.formatDescription
            guard CMFormatDescriptionGetMediaType(desc) == kCMMediaType_Video else { return nil }
            let dims = CMVideoFormatDescriptionGetDimensions(desc)
            let subType = CMFormatDescriptionGetMediaSubType(desc)
            let is420v = (subType == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
            let is420f = (subType == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
            // Skip 10-bit / ProRes / exotic pixel formats that force expensive conversion or HDR lookahead
            guard is420v || is420f else { return nil }

            let ranges = fmt.videoSupportedFrameRateRanges
            guard let maxFps = ranges.map(\.maxFrameRate).max(), maxFps >= 24.0 else { return nil }
            let supportsDesired = ranges.contains { $0.minFrameRate <= desiredFps + 0.01 && desiredFps <= $0.maxFrameRate + 0.01 }

            let exactMatch = (dims.width == reqW && dims.height == reqH)
            let tier: Int
            if exactMatch && supportsDesired {
                tier = 0
            } else if exactMatch {
                tier = 1
            } else if dims.width <= reqW && dims.height <= reqH && dims.width * 9 == dims.height * 16 && supportsDesired {
                tier = 2
            } else {
                return nil
            }

            var score = 0
            // Strongly prefer native 420v (zero-copy VideoToolbox IOSurface match)
            if is420v { score += 1000 }
            // Prefer exact-or-closest maxFrameRate (pick 60fps normal wide-FOV format over 240fps slow-mo crop!)
            if supportsDesired {
                let excessFps = max(0, Int(round(maxFps - desiredFps)))
                score -= excessFps * 6
            } else {
                score += Int(maxFps) * 10
            }
            // Prefer binned formats for 720p/1080p (faster sensor readout, lower rolling shutter & heat)
            if reqW <= 1920 && fmt.isVideoBinned { score += 120 }
            // Prefer SDR (non-HDR) to eliminate multi-frame HDR exposure fusion buffering
            if !fmt.isVideoHDRSupported { score += 60 }
            // If user requested EIS, prefer a format supporting that stabilization mode
            if targetStab != .off {
                let res = resolveAVStabilizationMode(for: targetStab, format: fmt)
                if res.resolvedLevel == targetStab { score += 200 }
            }
            // For fallback tier 2, prefer larger resolution closer to requested
            if tier == 2 { score += Int(dims.width) }
            return (tier, score, maxFps)
        }

        var best: (fmt: AVCaptureDevice.Format, tier: Int, score: Int, maxFps: Double)?
        for fmt in dev.formats {
            guard let cand = scoreFormat(fmt) else { continue }
            if let cur = best {
                if cand.tier < cur.tier || (cand.tier == cur.tier && cand.score > cur.score) {
                    best = (fmt, cand.tier, cand.score, cand.maxFps)
                }
            } else {
                best = (fmt, cand.tier, cand.score, cand.maxFps)
            }
        }
        guard let best else { return nil }
        let clampedFps = min(targetFps, max(24, Int(floor(best.maxFps))))
        return (best.fmt, clampedFps)
    }

    private nonisolated static func applyFormatAndFrameRate(
        to dev: AVCaptureDevice,
        format: AVCaptureDevice.Format?,
        targetFps: Int,
        hwZoom: CGFloat? = nil
    ) {
        do {
            try dev.lockForConfiguration()
            if let fmt = format, dev.activeFormat != fmt {
                dev.activeFormat = fmt
            }
            // Disable multi-frame Video HDR & Low-Light FPS drop to keep capture latency minimal and deterministic
            dev.automaticallyAdjustsVideoHDREnabled = false
            if dev.activeFormat.isVideoHDRSupported && dev.isVideoHDREnabled {
                dev.isVideoHDREnabled = false
            }
            if dev.isLowLightBoostSupported {
                dev.automaticallyEnablesLowLightBoostWhenAvailable = false
            }

            let desired = Double(max(15, targetFps))
            if dev.activeFormat.videoSupportedFrameRateRanges.contains(where: {
                $0.minFrameRate <= desired + 0.01 && desired <= $0.maxFrameRate + 0.01
            }) {
                let dur = CMTime(value: 1, timescale: CMTimeScale(targetFps))
                dev.activeVideoMinFrameDuration = dur
                dev.activeVideoMaxFrameDuration = dur
            }
            if let hwZoom {
                let clamped = max(dev.minAvailableVideoZoomFactor, min(dev.activeFormat.videoMaxZoomFactor, hwZoom))
                dev.videoZoomFactor = clamped
            }
            dev.unlockForConfiguration()
        } catch {}
    }

    private nonisolated static func applyFrameRate(_ targetFps: Int, to dev: AVCaptureDevice) {
        applyFormatAndFrameRate(to: dev, format: nil, targetFps: targetFps)
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

    private func configureAndStartCapture(reuseIfMatching: Bool = false) {
        let wantMultiCam = duoMode.isMultiCam && multiCamSession != nil
        let targetRes = resolution
        let targetLens = lens
        let wantFront = (targetLens == .front)
        let targetFps = fps
        let targetZoom = zoomFactor
        let targetStab = stabilizationLevel
        let targetOrientation = currentVideoOrientation()

        sessionQueue.async { [weak self] in
            guard let self else { return }

            // Avoid tearing down & flashing a running capture session if configuration already matches!
            if reuseIfMatching &&
                wantMultiCam == self.configuredMultiCam &&
                wantFront == self.configuredIsFront &&
                targetRes == self.configuredResolution &&
                targetFps == self.configuredFps {
                if wantMultiCam, let mc = self.multiCamSession, mc.isRunning {
                    Task { @MainActor [weak self] in self?.isPreviewActive = true }
                    return
                }
                if !wantMultiCam && self.singleSession.isRunning {
                    Task { @MainActor [weak self] in self?.isPreviewActive = true }
                    return
                }
            }

            if wantMultiCam, let mc = self.multiCamSession {
                if self.singleSession.isRunning { self.singleSession.stopRunning() }
                mc.beginConfiguration()
                for i in mc.inputs { mc.removeInput(i) }
                for o in mc.outputs { mc.removeOutput(o) }

                let backCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
                let frontCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
                var videoConn: AVCaptureConnection?
                if let backCam, let bIn = try? AVCaptureDeviceInput(device: backCam), mc.canAddInput(bIn) {
                    mc.addInputWithNoConnections(bIn)
                    let out = AVCaptureVideoDataOutput()
                    out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
                    out.alwaysDiscardsLateVideoFrames = true
                    out.setSampleBufferDelegate(self, queue: self.videoOutputQueue)
                    if mc.canAddOutput(out) {
                        mc.addOutputWithNoConnections(out)
                        if let port = bIn.ports(for: .video, sourceDeviceType: backCam.deviceType, sourceDevicePosition: .back).first {
                            let conn = AVCaptureConnection(inputPorts: [port], output: out)
                            if conn.isVideoOrientationSupported { conn.videoOrientation = targetOrientation }
                            if mc.canAddConnection(conn) {
                                mc.addConnection(conn)
                                videoConn = conn
                            }
                        }
                    }
                }
                if let frontCam, let fIn = try? AVCaptureDeviceInput(device: frontCam), mc.canAddInput(fIn) {
                    mc.addInputWithNoConnections(fIn)
                }
                mc.commitConfiguration()

                var appliedStab: StabilizationLevel = .off
                if let backCam {
                    Self.applyFrameRate(min(targetFps, 30), to: backCam)
                    if let conn = videoConn, conn.isVideoStabilizationSupported {
                        let res = Self.resolveAVStabilizationMode(for: targetStab, format: backCam.activeFormat)
                        conn.preferredVideoStabilizationMode = res.mode
                        appliedStab = Self.level(from: conn.activeVideoStabilizationMode, fallback: res.resolvedLevel)
                    }
                }
                mc.startRunning()

                self.primaryDevice = backCam
                self.secondaryDevice = frontCam
                self.isUsingVirtualRearDevice = false
                self.wideBaseZoomFactor = 1.0
                self.configuredMultiCam = true
                self.configuredIsFront = false
                self.configuredResolution = targetRes
                self.configuredFps = targetFps

                let supportedStab = backCam.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.isPreviewActive = true
                    self.minDisplayZoom = 1.0
                    self.maxDisplayZoom = 6.0
                    self.activeHardwareStabilization = appliedStab
                    self.supportedStabilizationLevels = supportedStab
                    self.applyManualCameraControls()
                }
            } else {
                if let mc = self.multiCamSession, mc.isRunning { mc.stopRunning() }

                // Fast atomic in-place format/FPS switch when the exact same camera device is already running
                // (wrapped in beginConfiguration/commitConfiguration so AVFoundation flushes cleanly with 0 lag!)
                if !self.configuredMultiCam &&
                    self.configuredIsFront == wantFront &&
                    self.singleSession.isRunning,
                   let cam = self.primaryDevice,
                   let picked = Self.selectOptimalFormat(for: cam, targetRes: targetRes, targetFps: targetFps, targetStab: targetStab) {
                    let desiredHwZoom = self.isUsingVirtualRearDevice ? (max(0.5, targetZoom) * self.wideBaseZoomFactor) : max(1.0, targetZoom)
                    let clampedHwZoom = max(cam.minAvailableVideoZoomFactor, min(min(picked.format.videoMaxZoomFactor, self.wideBaseZoomFactor * 10.0), desiredHwZoom))
                    let resolvedStab = Self.resolveAVStabilizationMode(for: targetStab, format: picked.format)

                    self.singleSession.beginConfiguration()
                    if self.singleSession.sessionPreset != .inputPriority {
                        self.singleSession.sessionPreset = .inputPriority
                    }
                    Self.applyFormatAndFrameRate(to: cam, format: picked.format, targetFps: picked.effectiveFps, hwZoom: clampedHwZoom)
                    for out in self.singleSession.outputs {
                        if let conn = out.connection(with: .video) {
                            if conn.isVideoOrientationSupported && conn.videoOrientation != targetOrientation {
                                conn.videoOrientation = targetOrientation
                            }
                            if conn.isVideoStabilizationSupported {
                                conn.preferredVideoStabilizationMode = resolvedStab.mode
                            }
                        }
                    }
                    self.singleSession.commitConfiguration()

                    var appliedStab: StabilizationLevel = resolvedStab.resolvedLevel
                    for out in self.singleSession.outputs {
                        if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                            appliedStab = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolvedStab.resolvedLevel)
                        }
                    }
                    self.configuredResolution = targetRes
                    self.configuredFps = targetFps
                    let supportedStab = Self.querySupportedStabilizationLevels(format: cam.activeFormat)
                    let maxDisp = min(10.0, max(2.0, cam.activeFormat.videoMaxZoomFactor / self.wideBaseZoomFactor))
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        self.isPreviewActive = true
                        self.maxDisplayZoom = maxDisp
                        self.activeHardwareStabilization = appliedStab
                        self.supportedStabilizationLevels = supportedStab
                        self.applyManualCameraControls()
                    }
                    return
                }

                self.singleSession.beginConfiguration()
                for i in self.singleSession.inputs { self.singleSession.removeInput(i) }

                let cam: AVCaptureDevice?
                let isVirtual: Bool
                let wideBase: CGFloat
                let minDisp: CGFloat

                if wantFront {
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

                let pickedFmt = cam.flatMap { Self.selectOptimalFormat(for: $0, targetRes: targetRes, targetFps: targetFps, targetStab: targetStab) }
                if pickedFmt != nil {
                    self.singleSession.sessionPreset = .inputPriority
                } else {
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
                }

                if self.singleSession.outputs.isEmpty {
                    let out = AVCaptureVideoDataOutput()
                    // Native 420v matches VtVideoEncoder's VideoToolbox IOSurface format directly (zero-copy!)
                    out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
                    out.alwaysDiscardsLateVideoFrames = true
                    out.setSampleBufferDelegate(self, queue: self.videoOutputQueue)
                    if self.singleSession.canAddOutput(out) { self.singleSession.addOutput(out) }
                }
                for out in self.singleSession.outputs {
                    if let conn = out.connection(with: .video), conn.isVideoOrientationSupported {
                        conn.videoOrientation = targetOrientation
                    }
                }
                var appliedStab: StabilizationLevel = .off
                if let cam {
                    let maxFmtZoom = pickedFmt?.format.videoMaxZoomFactor ?? cam.activeFormat.videoMaxZoomFactor
                    let desiredHwZoom = isVirtual ? (max(minDisp, targetZoom) * wideBase) : max(1.0, targetZoom)
                    let clampedHwZoom = max(cam.minAvailableVideoZoomFactor, min(min(maxFmtZoom, wideBase * 10.0), desiredHwZoom))
                    Self.applyFormatAndFrameRate(
                        to: cam,
                        format: pickedFmt?.format,
                        targetFps: pickedFmt?.effectiveFps ?? targetFps,
                        hwZoom: clampedHwZoom
                    )

                    let resolvedStab = Self.resolveAVStabilizationMode(for: targetStab, format: cam.activeFormat)
                    for out in self.singleSession.outputs {
                        if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                            conn.preferredVideoStabilizationMode = resolvedStab.mode
                        }
                    }
                    appliedStab = resolvedStab.resolvedLevel
                }
                self.singleSession.commitConfiguration()

                if let cam {
                    let resolvedStab = Self.resolveAVStabilizationMode(for: targetStab, format: cam.activeFormat)
                    for out in self.singleSession.outputs {
                        if let conn = out.connection(with: .video), conn.isVideoStabilizationSupported {
                            appliedStab = Self.level(from: conn.activeVideoStabilizationMode, fallback: resolvedStab.resolvedLevel)
                        }
                    }
                }

                if !self.singleSession.isRunning {
                    self.singleSession.startRunning()
                }

                self.primaryDevice = cam
                self.secondaryDevice = nil
                self.isUsingVirtualRearDevice = isVirtual
                self.wideBaseZoomFactor = wideBase
                self.configuredMultiCam = false
                self.configuredIsFront = wantFront
                self.configuredResolution = targetRes
                self.configuredFps = targetFps

                let supportedStab = cam.map { Self.querySupportedStabilizationLevels(format: $0.activeFormat) } ?? StabilizationLevel.allCases
                let maxDisp = cam.map { min(10.0, max(2.0, $0.activeFormat.videoMaxZoomFactor / wideBase)) } ?? 10.0
                Task { @MainActor [weak self] in
                    guard let self else { return }
                    self.isPreviewActive = true
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
        let nextTemp: Int
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: nextTemp = 34
        case .fair: nextTemp = 40
        case .serious: nextTemp = 46
        case .critical: nextTemp = 52
        @unknown default: nextTemp = 36
        }
        if telemetry.tempC != nextTemp {
            telemetry.tempC = nextTemp
        }
    }

    // MARK: - Telemetry Timer (Single Batched Struct Assignment at 4 Hz!)

    private func startTelemetryTimer() {
        tickTimer?.invalidate()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.onTelemetryTick() }
        }
    }

    private func onTelemetryTick() {
        var next = telemetry

        let elapsed = max(0.0, CACurrentMediaTime() - streamStartWallTime)
        let wholeSecs = Int(elapsed) + 3600
        let hh = (wholeSecs / 3600) % 24
        let mm = (wholeSecs / 60) % 60
        let ss = wholeSecs % 60
        let frac = elapsed - floor(elapsed)
        let ff = min(max(0, fps - 1), Int(frac * Double(max(1, fps))))
        next.timecode = String(format: "%02d:%02d:%02d:%02d", hh, mm, ss, ff)

        let rawBat = UIDevice.current.batteryLevel
        next.batteryPct = rawBat >= 0 ? Int(rawBat * 100) : 92
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: next.tempC = 34
        case .fair: next.tempC = 40
        case .serious: next.tempC = 46
        case .critical: next.tempC = 52
        @unknown default: next.tempC = 36
        }

        if isDemoMode {
            demoPhaseCounter += 1
            let phase = Double(demoPhaseCounter) * 0.24
            next.bitrateMbps = max(2.0, bitrateMbps + sin(phase) * 0.9)
            let stabAdded = Double(stabilizationLevel.latencyHintMs) * 0.15
            next.latencyMs = 7.4 + stabAdded + cos(phase * 0.7) * 0.5
            next.jitterMs = 0.28 + abs(sin(phase * 0.5)) * 0.12
            next.lossPct = 0.0
            next.bufferPct = 98
            next.actualFps = Double(fps)
            next.micPeakDbfs = (isIntercomActive && (pttActive || latchActive)) ? Float(-10.0 + sin(phase * 2.2) * 7.0) : (isIntercomActive ? Float(-44.0 + sin(phase) * 4.0) : -60.0)
            next.ifbPeakDbfs = (isIntercomActive && ifbEnabled && directorSpeaking) ? Float(-12.0 + cos(phase * 1.9) * 6.0) : (isIntercomActive ? Float(-48.0 + cos(phase) * 3.0) : -60.0)
            next.histogram = (0..<24).map { i in
                let x = Double(i) / 23.0
                let shift = Double(exposureBias) * 0.09
                let bell = exp(-pow((x - (0.48 + shift)) * 3.1, 2.0)) * 0.82
                let ripple = sin(phase + Double(i) * 0.45) * 0.06
                return Float(max(0.05, min(0.98, bell + ripple)))
            }
            next.history.append(TelemetrySample(bitrateMbps: next.bitrateMbps, latencyMs: next.latencyMs, dropout: false))
            if next.history.count > 42 { next.history.removeFirst(next.history.count - 42) }
            telemetry = next
            return
        }

        // Read audio peak levels without ANY thread hops!
        next.micPeakDbfs = intercomAudio.micPeakDbfs
        next.ifbPeakDbfs = intercomAudio.ifbPeakDbfs

        var st = LV_StreamStats()
        var gotStats = false
        if senderLock.try() {
            if let s = senderHandle {
                gotStats = (lv_sender_get_stream_stats(s, &st) == LV_OK.rawValue)
            }
            senderLock.unlock()
        }

        if gotStats {
            captureLagLock.lock()
            let capLag = _smoothedCaptureLagMs
            captureLagLock.unlock()
            let totalLocalMs = st.encode_ms + ((capLag > 0.0 && capLag < 5000.0) ? capLag : 0.0)
            next.bitrateMbps = st.bitrate_mbps
            next.latencyMs = totalLocalMs
            next.actualFps = st.fps
            next.codecBackend = withUnsafePointer(to: st.codec_backend) {
                $0.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
            }
            next.history.append(TelemetrySample(bitrateMbps: st.bitrate_mbps, latencyMs: totalLocalMs, dropout: st.fps < Double(fps) * 0.85))
            if next.history.count > 42 { next.history.removeFirst(next.history.count - 42) }
        } else if !isStreaming {
            next.bitrateMbps = 0.0
            next.latencyMs = 0.0
            next.actualFps = 0.0
        }

        // Single atomic SwiftUI @Published update per tick!
        telemetry = next

        let bat = next.batteryPct
        let tmp = next.tempC
        tallyLock.lock()
        if let th = tallyHandle {
            lv_tally_intercom_update_camera_telemetry(th, UInt8(clamping: bat), Int8(clamping: tmp))
        }
        tallyLock.unlock()
    }
}

extension CameraEngine: AVCaptureVideoDataOutputSampleBufferDelegate {
    nonisolated func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        // Measure true AVFoundation capture-to-delegate pipeline delay (exposes Apple EIS lookahead buffer!)
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        if CMTIME_IS_NUMERIC(pts) {
            let clk = (configuredMultiCam ? multiCamSession?.synchronizationClock : singleSession.synchronizationClock)
                ?? CMClockGetHostTimeClock()
            let lagMs = CMTimeGetSeconds(CMTimeSubtract(CMClockGetTime(clk), pts)) * 1000.0
            if lagMs.isFinite && lagMs >= 0.0 && lagMs < 5000.0 && captureLagLock.try() {
                _smoothedCaptureLagMs = (_smoothedCaptureLagMs <= 0.1) ? lagMs : (_smoothedCaptureLagMs * 0.82 + lagMs * 0.18)
                captureLagLock.unlock()
            }
        }

        let w = Int32(CVPixelBufferGetWidth(pb))
        let h = Int32(CVPixelBufferGetHeight(pb))
        // Pass timestamp_us = 0 (identical to Android JNI) so LanVidCore stamps lv::now_us32()
        // in the exact same steady_clock domain as Ping/Pong RTT sync and VideoPlayoutBuffer!
        let tsUs: UInt32 = 0

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
