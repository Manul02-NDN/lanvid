import AVFoundation
import Foundation
import LanVidCore
import SwiftUI
import UIKit

/// Duplex Intercom audio controller (AVAudioSession playAndRecord @ 48 kHz, 5 ms buffer, background-safe).
final class IntercomAudioController {
    private let audioEngine = AVAudioEngine()
    private var isRunning = false
    var onMicFloat: ((UnsafePointer<Float>, Int) -> Void)?
    var onMicLevel: ((Float) -> Void)?

    func start() {
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

            let input = audioEngine.inputNode
            let format = input.outputFormat(forBus: 0)
            guard format.channelCount > 0 else { return }
            input.installTap(onBus: 0, bufferSize: 480, format: format) { [weak self] buffer, _ in
                guard let ch = buffer.floatChannelData?[0] else { return }
                let count = Int(buffer.frameLength)
                var peak: Float = 0.00001
                for i in 0..<count {
                    let a = abs(ch[i])
                    if a > peak { peak = a }
                }
                let dbfs = max(-60.0, 20.0 * log10(peak))
                self?.onMicLevel?(dbfs)
                self?.onMicFloat?(ch, count)
            }
            audioEngine.prepare()
            try audioEngine.start()
            isRunning = true
        } catch {
            // Hardware audio session unavailable (e.g. Simulator)
        }
    }

    func stop() {
        guard isRunning else { return }
        audioEngine.inputNode.removeTap(onBus: 0)
        audioEngine.stop()
        isRunning = false
    }
}

/// Complete Pro Cine Camera, Hardware VideoToolbox / LanVidCore Sender, Tally + Duplex Intercom,
/// AVCaptureMultiCamSession Dual-Cam (PIP / 50-50 Split), iPhone Duo Sync, and Demo Cine Studio engine.
@MainActor
final class CameraEngine: NSObject, ObservableObject {
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
    @Published var isDemoMode = false
    @Published var statusText = ""

    var displayBadgeTag: String {
        if !assignedCamTag.isEmpty { return assignedCamTag }
        if assignedCamNumber > 0 { return "CAM \(assignedCamNumber)" }
        return cameraLabel.isEmpty ? "CAM 1" : cameraLabel
    }

    // MARK: - Tally & Duplex Intercom
    @Published var tallyState: ProTallyState = .standby
    @Published var remoteRec = false
    @Published var directorSpeaking = false
    @Published var duckingGain: Float = 1.0
    @Published var pttActive = false { didSet { syncIntercomState() } }
    @Published var latchActive = false {
        didSet {
            if latchActive { pttActive = true }
            syncIntercomState()
        }
    }
    @Published var ifbEnabled = true { didSet { syncIntercomState() } }
    @Published var ifbVolume: Float = 0.85 { didSet { syncIntercomState() } }

    // MARK: - Manual Pro Cine Camera Controls (Primary Back / Active Lens)
    @Published var lens: CineLens = .wide
    @Published var shutterAngle: Int = 180
    @Published var isoValue: Float = 400
    @Published var autoExposure = true
    @Published var wbKelvin: Float = 5600
    @Published var wbTint: Float = 0
    @Published var autoWhiteBalance = true
    @Published var focusPosition: Float = 0.45
    @Published var focusPointA: Float = 0.25
    @Published var focusPointB: Float = 0.78
    @Published var autoFocus = true
    @Published var focusPeaking = false
    @Published var zebra = false
    @Published var showGuides = true
    @Published var showHistogram = true

    // MARK: - Secondary Camera Exposure (Dual-Cam PIP / Split)
    @Published var secondaryAutoExposure = true
    @Published var secondaryExposureBias: Float = 0.0

    // MARK: - Encoder & Stream Profile
    @Published var resolution: CineResolution = .fhd1080 {
        didSet { if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() } }
    }
    @Published var fps: Int = 60 {
        didSet { if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() } }
    }
    @Published var codec: CineCodec = .hevc {
        didSet { if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() } }
    }
    @Published var bitrateMbps: Double = 35.0 {
        didSet {
            if isStreaming && !isDemoMode {
                senderLock.lock()
                let s = senderHandle
                senderLock.unlock()
                if let s {
                    lv_sender_set_bitrate(s, UInt32(max(2000, Int(bitrateMbps * 1000.0))))
                }
            }
            if !isApplyingRemoteSync { sendLocalConfigSyncToStudio() }
        }
    }
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

    // MARK: - AVFoundation Sessions
    nonisolated let singleSession = AVCaptureSession()
    nonisolated let multiCamSession: AVCaptureMultiCamSession? = AVCaptureMultiCamSession.isMultiCamSupported ? AVCaptureMultiCamSession() : nil
    private let captureQueue = DispatchQueue(label: "com.manul02.lanvid.camera.capture", qos: .userInteractive)
    private var primaryDevice: AVCaptureDevice?
    private var secondaryDevice: AVCaptureDevice?

    // MARK: - LanVidCore C ABI Handles
    private let senderLock = NSLock()
    private nonisolated(unsafe) var senderHandle: OpaquePointer?
    private var tallyHandle: OpaquePointer?
    private let intercomAudio = IntercomAudioController()
    private var tickTimer: Timer?
    private var frameCounter: Int = 0
    private let cameraId: UInt32 = UInt32.random(in: 1000...999999)

    override init() {
        let ud = UserDefaults.standard
        self.destIp = ud.string(forKey: "cam_dest_ip") ?? "192.168.1.24"
        let vp = ud.integer(forKey: "cam_video_port")
        self.videoPort = vp > 0 ? vp : 5006
        let tp = ud.integer(forKey: "cam_tally_port")
        self.tallyPort = tp > 0 ? tp : 5022
        self.cameraLabel = ud.string(forKey: "cam_label") ?? "CAM A"
        self.assignedCamNumber = ud.integer(forKey: "cam_assigned_number")
        self.assignedCamTag = ud.string(forKey: "cam_assigned_tag") ?? ""
        super.init()

        lv_init()
        UIDevice.current.isBatteryMonitoringEnabled = true
        let simd = lv_get_cpu_simd_info().map { String(cString: $0) } ?? "NEON"
        self.statusText = tr("Готово • ", "Ready • ") + simd

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
        intercomAudio.onMicFloat = { [weak self] ptr, count in
            guard let self, let th = self.tallyHandle else { return }
            if self.pttActive || self.latchActive {
                lv_intercom_push_mic_pcm(th, ptr, Int32(count))
            }
        }
    }

    deinit {
        tickTimer?.invalidate()
        intercomAudio.stop()
        if let s = senderHandle {
            lv_sender_stop(s)
            lv_sender_destroy(s)
        }
        if let t = tallyHandle {
            lv_tally_intercom_destroy(t)
        }
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
    }

    /// Launches a rich interactive Demo Cine Studio (no PC or hardware camera required) for App Store Review & exploration.
    func startDemoCineStudio(tally: ProTallyState = .preview, mode: DuoMode = .singleHUD) {
        stopStreaming()
        isDemoMode = true
        isStreaming = true
        duoMode = mode
        tallyState = tally
        remoteRec = (tally == .program)
        directorSpeaking = (tally == .program)
        resolution = .uhd4k
        fps = 60
        codec = .hevc
        bitrateMbps = 35.0
        shutterAngle = 180
        isoValue = 400
        wbKelvin = 5600
        autoExposure = false
        autoWhiteBalance = false
        autoFocus = false
        focusPosition = 0.48
        telemetry.codecBackend = "VideoToolbox H.265 (HW)"
        telemetry.bitrateMbps = 35.2
        telemetry.latencyMs = 7.8
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
        stopStreaming()
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
            cfg.fec_group_size = 6
            cfg.adaptive_quality = 1
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
        }

        // Start Tally & Duplex Intercom engine on port 5022
        let th = destIp.withCString { ipC in
            cameraLabel.withCString { lblC in
                lv_tally_intercom_create_camera(ipC, UInt16(clamping: tallyPort), cameraId, lblC)
            }
        }
        if let th {
            tallyHandle = th
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
            syncIntercomState()
        }

        if duoMode != .duoCompanion {
            configureAndStartCapture()
        }
        intercomAudio.start()
        isStreaming = true
        statusText = "\(resolution.rawValue) \(fps)p \(codec.shortName) → \(destIp):\(videoPort)"
        startTelemetryTimer()
    }

    func stopStreaming() {
        tickTimer?.invalidate()
        tickTimer = nil
        captureQueue.async { [singleSession, multiCamSession] in
            if singleSession.isRunning { singleSession.stopRunning() }
            if let mc = multiCamSession, mc.isRunning { mc.stopRunning() }
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
        if let t = tallyHandle {
            lv_tally_intercom_destroy(t)
            tallyHandle = nil
        }
        isStreaming = false
        if !isDemoMode {
            tallyState = .standby
            remoteRec = false
            directorSpeaking = false
        }
    }

    // MARK: - iPhone Duo Mode Switching & Companion Remote Control

    func setDuoMode(_ newMode: DuoMode) {
        duoMode = newMode
        if newMode == .duoCompanion {
            duoRole = .companion
        } else if duoRole == .companion {
            duoRole = .camA
        }
        if isStreaming && !isDemoMode {
            configureAndStartCapture()
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
        if duoMode == .duoCompanion { sendCompanionCommandToSelectedPeer() }
    }

    func selectLens(_ newLens: CineLens) {
        lens = newLens
        if isStreaming && !isDemoMode && !duoMode.isMultiCam {
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
                    sec.setExposureTargetBias(secondaryExposureBias, completionHandler: nil)
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
            } else if dev.isExposureModeSupported(.custom) {
                let denom = Double(fps) * (360.0 / Double(max(1, shutter)))
                let dur = CMTime(value: 1, timescale: CMTimeScale(max(24, Int(denom))))
                let clampedIso = max(dev.activeFormat.minISO, min(dev.activeFormat.maxISO, iso))
                dev.setExposureModeCustom(duration: dur, iso: clampedIso)
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
        } catch {
            // Device configuration locked or unavailable
        }
    }

    private func applyRemoteStudioControl(_ ctrl: LV_CameraControl) {
        switch ctrl.lens {
        case 1: lens = .ultraWide
        case 2: lens = .tele
        case 3: lens = .front
        default: lens = .wide
        }
        autoFocus = (ctrl.focus_auto != 0)
        if !autoFocus {
            focusPosition = Float(min(1000, ctrl.focus_pos)) / 1000.0
        }
        applyManualCameraControls()
    }

    private func applyRemoteVideoConfigSync(_ cfg: LV_VideoConfigSync) {
        if (cfg.flags & 0x01) != 0 {
            senderLock.lock()
            let s = senderHandle
            senderLock.unlock()
            if let s { lv_sender_force_keyframe(s) }
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
        bitrateMbps = min(150.0, max(5.0, targetMbps))

        if needLensSwitch {
            selectLens(targetLens)
        }
        if needEncoderRestart && isStreaming && !isDemoMode {
            reconfigureVideoSenderOnly()
        } else if isStreaming && !isDemoMode {
            senderLock.lock()
            let s = senderHandle
            senderLock.unlock()
            if let s { lv_sender_set_bitrate(s, UInt32(max(2000, Int(bitrateMbps * 1000.0)))) }
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
        cfg.fec_group_size = 6
        cfg.adaptive_quality = 1
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
        guard let th = tallyHandle else { return }
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
            case .tele: return 2
            case .front: return 3
            case .wide: return 0
            }
        }()
        cfg.flags = 0x02  // kVideoConfigFlagFromCamera
        let tagBytes = Array(displayBadgeTag.utf8.prefix(15))
        withUnsafeMutableBytes(of: &cfg.assigned_cam_tag) { raw in
            raw.initializeMemory(as: UInt8.self, repeating: 0)
            for (i, b) in tagBytes.enumerated() { raw[i] = b }
        }
        _ = lv_tally_intercom_send_video_config_sync(th, &cfg)
    }

    private func syncIntercomState() {
        guard let th = tallyHandle else { return }
        lv_intercom_set_state(th, (pttActive || latchActive) ? 1 : 0, ifbEnabled ? 0 : 1, ifbVolume, 0.0)
    }

    // MARK: - AVFoundation Capture Setup (Single + AVCaptureMultiCamSession)

    private func configureAndStartCapture() {
        let wantMultiCam = duoMode.isMultiCam && multiCamSession != nil
        let targetRes = resolution
        let targetLens = lens

        captureQueue.async { [weak self] in
            guard let self else { return }
            if wantMultiCam, let mc = self.multiCamSession {
                if self.singleSession.isRunning { self.singleSession.stopRunning() }
                mc.beginConfiguration()
                for i in mc.inputs { mc.removeInput(i) }
                for o in mc.outputs { mc.removeOutput(o) }

                let backCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
                let frontCam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
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
                            if mc.canAddConnection(conn) { mc.addConnection(conn) }
                        }
                    }
                }
                if let frontCam, let fIn = try? AVCaptureDeviceInput(device: frontCam), mc.canAddInput(fIn) {
                    mc.addInputWithNoConnections(fIn)
                }
                mc.commitConfiguration()
                mc.startRunning()
                Task { @MainActor in
                    self.primaryDevice = backCam
                    self.secondaryDevice = frontCam
                    self.applyManualCameraControls()
                }
            } else {
                if let mc = self.multiCamSession, mc.isRunning { mc.stopRunning() }
                self.singleSession.beginConfiguration()
                switch targetRes {
                case .uhd4k: self.singleSession.sessionPreset = .hd4K3840x2160
                case .fhd1080: self.singleSession.sessionPreset = .hd1920x1080
                case .hd720: self.singleSession.sessionPreset = .hd1280x720
                }
                for i in self.singleSession.inputs { self.singleSession.removeInput(i) }
                let pos: AVCaptureDevice.Position = (targetLens == .front) ? .front : .back
                let devType: AVCaptureDevice.DeviceType = {
                    switch targetLens {
                    case .ultraWide: return .builtInUltraWideCamera
                    case .tele: return .builtInTelephotoCamera
                    default: return .builtInWideAngleCamera
                    }
                }()
                let cam = AVCaptureDevice.default(devType, for: .video, position: pos)
                    ?? AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: pos)
                if let cam, let input = try? AVCaptureDeviceInput(device: cam), self.singleSession.canAddInput(input) {
                    self.singleSession.addInput(input)
                }
                if self.singleSession.outputs.isEmpty {
                    let out = AVCaptureVideoDataOutput()
                    out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
                    out.alwaysDiscardsLateVideoFrames = true
                    out.setSampleBufferDelegate(self, queue: self.captureQueue)
                    if self.singleSession.canAddOutput(out) { self.singleSession.addOutput(out) }
                }
                self.singleSession.commitConfiguration()
                self.singleSession.startRunning()
                Task { @MainActor in
                    self.primaryDevice = cam
                    self.secondaryDevice = nil
                    self.applyManualCameraControls()
                }
            }
        }
    }

    // MARK: - Telemetry & Tally Polling Timer

    private func startTelemetryTimer() {
        tickTimer?.invalidate()
        tickTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.onTelemetryTick() }
        }
    }

    private func onTelemetryTick() {
        frameCounter += 6 // 10 Hz tick at 60 fps
        let totalSecs = 3600 + frameCounter / max(1, fps)
        let hh = (totalSecs / 3600) % 24
        let mm = (totalSecs / 60) % 60
        let ss = totalSecs % 60
        let ff = frameCounter % max(1, fps)
        telemetry.timecode = String(format: "%02d:%02d:%02d:%02d", hh, mm, ss, ff)

        let rawBat = UIDevice.current.batteryLevel
        telemetry.batteryPct = rawBat >= 0 ? Int(rawBat * 100) : 92

        if isDemoMode {
            let phase = Double(frameCounter) * 0.04
            telemetry.bitrateMbps = bitrateMbps + sin(phase) * 0.8
            telemetry.latencyMs = 7.4 + cos(phase * 0.7) * 0.6
            telemetry.actualFps = Double(fps)
            telemetry.micPeakDbfs = (pttActive || latchActive) ? Float(-10.0 + sin(phase * 2.2) * 6.0) : -46.0
            telemetry.ifbPeakDbfs = (ifbEnabled && directorSpeaking) ? Float(-12.0 + cos(phase * 1.9) * 5.0) : -52.0
            telemetry.histogram = (0..<24).map { i in
                let x = Double(i) / 23.0
                let bell = exp(-pow((x - 0.48) * 3.1, 2.0)) * 0.82
                let ripple = sin(phase + Double(i) * 0.45) * 0.06
                return Float(max(0.05, min(0.98, bell + ripple)))
            }
            return
        }

        senderLock.lock()
        let activeSender = senderHandle
        senderLock.unlock()
        if let s = activeSender {
            var st = LV_StreamStats()
            if lv_sender_get_stream_stats(s, &st) == LV_OK.rawValue {
                telemetry.bitrateMbps = st.bitrate_mbps
                telemetry.latencyMs = st.encode_ms
                telemetry.actualFps = st.fps
                telemetry.codecBackend = withUnsafePointer(to: st.codec_backend) {
                    $0.withMemoryRebound(to: CChar.self, capacity: 64) { String(cString: $0) }
                }
            }
        }
        if let th = tallyHandle {
            lv_tally_intercom_poll(th)
            directorSpeaking = (lv_tally_intercom_is_director_speaking(th) != 0)
            duckingGain = lv_tally_intercom_ducking_gain(th)
            lv_tally_intercom_update_camera_telemetry(th, UInt8(clamping: telemetry.batteryPct), Int8(clamping: telemetry.tempC))
        }
    }
}

extension CameraEngine: AVCaptureVideoDataOutputSampleBufferDelegate {
    nonisolated func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }

        let w = Int32(CVPixelBufferGetWidth(pb))
        let h = Int32(CVPixelBufferGetHeight(pb))
        guard let p0 = CVPixelBufferGetBaseAddressOfPlane(pb, 0)?.assumingMemoryBound(to: UInt8.self),
              let p1 = CVPixelBufferGetBaseAddressOfPlane(pb, 1)?.assumingMemoryBound(to: UInt8.self) else { return }
        let s0 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 0))
        let s1 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pb, 1))
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let tsUs = UInt32(max(0, Int64(CMTimeGetSeconds(pts) * 1_000_000.0)) & 0xFFFFFFFF)

        senderLock.lock()
        defer { senderLock.unlock() }
        guard let s = senderHandle else { return }
        _ = lv_sender_push_cvpixelbuffer_planes(s, p0, s0, p1, s1, w, h, Int32(LV_PIXEL_FORMAT_NV12.rawValue), tsUs)
    }
}
