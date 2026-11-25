import AVFoundation
import Foundation
import LanVidCore

/// Talks to LanVid Studio through the LanVid core C ABI (lv_monitor_client_*): kMonitorControl up,
/// 2-channel AUDIO2 down. Mixing and all DSP run on the PC; the phone only plays the finished stereo bus.
///
/// Threads: the network loop (poll every 1 ms, VU/meta 15 Hz, keepalive 1 s) runs on its own thread;
/// the AVAudioSourceNode render block pulls stereo straight from the core's playout buffer with
/// preallocated memory only (no Swift object access, no allocation on the audio thread).
@MainActor
final class MonitorEngine: ObservableObject {
    @Published private(set) var connected: StudioEndpoint?
    @Published private(set) var channels: [ChannelStrip] = []
    @Published private(set) var telemetry = MonitorTelemetry()
    @Published private(set) var audioStatus = ""
    @Published var lastError: String?
    /// Demo Studio Preview: a simulated 8-channel studio, no network and no audio - lets everyone (App Review
    /// included) explore every screen without a PC running LanVid Studio.
    @Published private(set) var isDemo = false

    @Published var quality: StreamQuality = .k256 {
        didSet {
            if quality != oldValue {
                if !wifiRedundancyUserSet {
                    wifiRedundancy = (quality == .k192 || quality == .k256)
                }
                pushStream()
                save()
            }
        }
    }
    @Published var buffer: MonitorBuffer = .ms10 { didSet { if buffer != oldValue { pushStream(); save() } } }
    @Published var wifiRedundancy: Bool = true {
        didSet {
            if wifiRedundancy != oldValue {
                pushWifiRedundancy()
                save()
            }
        }
    }
    @Published var dsp = DspSettings() { didSet { if dsp != oldValue { pushDsp(); save() } } }
    @Published var clientName: String = UIDeviceName.current { didSet { save() } }
    @Published private(set) var networkInterfaces: [NetInterfaceOption] = []
    @Published var primaryNicIp: String = "" { didSet { if primaryNicIp != oldValue { pushNetworkInterfaces(); save() } } }
    @Published var secondaryNicIp: String = "" { didSet { if secondaryNicIp != oldValue { pushNetworkInterfaces(); save() } } }

    private var wifiRedundancyUserSet = false

    private var cli: OpaquePointer?
    private var ioThread: Thread?
    private let io = IoState()
    private let engine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?
    private let scratchFrames = 8192
    private let scratch: UnsafeMutablePointer<Float>
    private var observers: [NSObjectProtocol] = []
    private var demoTask: Task<Void, Never>?
    static let demoStudio = StudioEndpoint(name: "Demo Studio", host: "demo", port: 0)

    init() {
        scratch = .allocate(capacity: 8192 * 2)
        scratch.initialize(repeating: 0, count: 8192 * 2)
        load()
        refreshNetworkInterfaces()
        observers.append(NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
                Task { @MainActor in self?.handleInterruption(n) }
            })
        observers.append(NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.restartAudio() }  // headphones plugged / unplugged, route change
            })
    }

    deinit {
        scratch.deallocate()
    }

    // MARK: - Connection

    func connect(to studio: StudioEndpoint) {
        if studio == Self.demoStudio { startDemo(); return }
        disconnect()
        guard let h = lv_monitor_client_create(48000) else {
            lastError = tr("Не вдалося створити клієнт", "Could not create the monitor client")
            return
        }
        lv_monitor_client_set_wifi_redundancy(h, wifiRedundancy ? 1 : 0)
        lv_monitor_client_set_network_interfaces(h, primaryNicIp, secondaryNicIp)
        let rc = lv_monitor_client_connect(h, studio.host, studio.port, String(clientName.prefix(15)),
                                           quality.wireKbps, UInt16(buffer.rawValue))
        guard rc == 0 else {
            lv_monitor_client_destroy(h)
            lastError = tr("Немає з'єднання з ", "Cannot connect to ") + "\(studio.host):\(studio.port)"
            return
        }
        cli = h
        connectStartedAt = Date()
        handshakeConfirmed = false
        connected = studio
        lastError = nil
        UserDefaults.standard.set(studio.host, forKey: "lastHost")
        UserDefaults.standard.set(Int(studio.port), forKey: "lastPort")
        pushDsp()
        for ch in channels { pushMix(ch) }
        pushSubscribedMask()
        startIo(h)
        startAudio(h)
    }

    func disconnect() {
        if isDemo { stopDemo(); return }
        stopAudio()
        io.running = false
        ioThread = nil
        if let h = cli {
            io.waitStopped()
            lv_monitor_client_disconnect(h)
            lv_monitor_client_destroy(h)
        }
        cli = nil
        connected = nil
        telemetry = MonitorTelemetry()
    }

    // MARK: - Demo Studio Preview

    private static let demoChannels = ["Kick", "Snare", "Bass", "Guitar L", "Guitar R", "Keys", "Vocal", "Click"]

    func startDemo() {
        if connected != nil { disconnect() }
        isDemo = true
        lastError = nil
        let saved = loadMix()
        channels = Self.demoChannels.enumerated().map { i, n in
            var s = saved[i] ?? ChannelStrip(id: i, name: n)
            s.name = n
            return s
        }
        connected = Self.demoStudio
        audioStatus = tr("Демо · без звуку", "Demo · no audio")
        let t0 = Date()
        demoTask?.cancel()
        demoTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                self?.tickDemo(Date().timeIntervalSince(t0))
                try? await Task.sleep(nanoseconds: 66_000_000)
            }
        }
    }

    /// Mix + processing used for the App Store screenshots (launch argument -LVMarketing).
    func applyMarketingPreset() {
        guard isDemo else { return }
        let gains: [Float] = [0, -2, -1, -6, -6, -4, 2.5, -12]
        let pans: [Float] = [0, 0.1, 0, -0.55, 0.55, 0.3, 0, 0]
        for i in channels.indices {
            channels[i].gainDb = gains[i]
            channels[i].pan = pans[i]
            channels[i].mute = (i == 7)  // the click track is muted
            channels[i].solo = false
        }
        var d = DspSettings()
        d.hpf = .hz80
        d.eqOn = true
        d.eqLowDb = 2; d.eqMidDb = -3; d.eqMidHz = 2500; d.eqHighDb = 1.5
        d.compOn = true; d.compThresholdDb = -26; d.compRatio = 4; d.compMakeupDb = 3
        d.masterVolumeDb = -3
        dsp = d
        quality = .k256
        buffer = .ms5
    }

    func stopDemo() {
        demoTask?.cancel()
        demoTask = nil
        isDemo = false
        connected = nil
        channels = []
        telemetry = MonitorTelemetry()
        audioStatus = ""
    }

    /// Deterministic, musical-looking levels: every channel breathes at its own rate.
    private func tickDemo(_ t: Double) {
        var mix = -120.0
        var level = [Double](repeating: -120, count: channels.count)
        for i in channels.indices {
            let f = 0.6 + 0.37 * Double(i)
            let env = 0.5 + 0.5 * sin(t * f * 2 + Double(i) * 1.7)
            let hit = pow(max(0, sin(t * (1.1 + 0.2 * Double(i)) * 3.1)), 3)
            level[i] = -34 + 30 * (0.55 * env + 0.45 * hit)
            channels[i].peakDbfs = Float(level[i])
            let ch = channels[i]
            let audible = !ch.mute && (!channels.contains { $0.solo } || ch.solo)
            if audible { mix = max(mix, level[i] + Double(ch.gainDb) + 3 * Double(abs(ch.pan) < 0.01 ? 0 : 1)) }
        }
        let master = mix + Double(dsp.masterVolumeDb) + (dsp.compOn ? Double(dsp.compMakeupDb) : 0)
        var gr = 0.0
        if dsp.compOn {
            let over = max(0, mix - Double(dsp.compThresholdDb))
            gr = -min(24, over * (1 - 1 / Double(max(1, dsp.compRatio))))
        }
        let out = dsp.masterMute ? -120 : min(-0.5, master + gr)
        let anySolo = channels.contains { $0.solo }
        let activeCh = UInt32(channels.filter { !$0.mute && (!anySolo || $0.solo) }.count)
        telemetry = MonitorTelemetry(gainReductionDb: Float(gr), mixPeakDbfs: Float(out),
                                     bufferMs: Double(buffer.rawValue) + 0.1, audioPackets: UInt64(t * 200),
                                     receiving: true, clockLocked: true, clockOffsetJitterUs: 180,
                                     duplicatesRecovered: wifiRedundancy ? UInt64(t * 3) : 0,
                                     activeFlowChannels: activeCh,
                                     effectivePlayoutMs: Double(buffer.rawValue) + 0.1,
                                     wifiRedundancyEnabled: wifiRedundancy)
    }

    // MARK: - Mixer (sent instantly as kMonitorControl + Rx Subscription mask)

    func setWifiRedundancyFromUser(_ enabled: Bool) {
        wifiRedundancyUserSet = true
        wifiRedundancy = enabled
        save()
    }

    func update(_ strip: ChannelStrip) {
        guard let i = channels.firstIndex(where: { $0.id == strip.id }) else { return }
        var s = strip
        s.peakDbfs = channels[i].peakDbfs
        guard channels[i] != s else { return }
        let subChanged = (channels[i].mute != s.mute) || (channels[i].solo != s.solo)
        channels[i] = s
        pushMix(s)
        if subChanged { pushSubscribedMask() }
        saveMix()
    }

    func resetMix() {
        for i in channels.indices {
            channels[i].gainDb = 0; channels[i].pan = 0; channels[i].mute = false; channels[i].solo = false
            pushMix(channels[i])
        }
        pushSubscribedMask()
        saveMix()
    }

    private func computeSubscribedMask() -> UInt64 {
        guard !channels.isEmpty else { return UInt64.max }
        let anySolo = channels.contains { $0.solo }
        var mask: UInt64 = 0
        for ch in channels where ch.id >= 0 && ch.id < 64 {
            let active = anySolo ? (ch.solo && !ch.mute) : !ch.mute
            if active { mask |= (UInt64(1) << UInt64(ch.id)) }
        }
        return mask
    }

    private func pushSubscribedMask() {
        guard let h = cli else { return }
        lv_monitor_client_set_subscribed_mask(h, computeSubscribedMask())
    }

    private func pushWifiRedundancy() {
        guard let h = cli else { return }
        lv_monitor_client_set_wifi_redundancy(h, wifiRedundancy ? 1 : 0)
    }

    private func pushNetworkInterfaces() {
        guard let h = cli else { return }
        lv_monitor_client_set_network_interfaces(h, primaryNicIp, secondaryNicIp)
    }

    func refreshNetworkInterfaces() {
        var raw = [LvNetInterfaceInfo](repeating: LvNetInterfaceInfo(), count: 16)
        let n = Int(lv_net_list_interfaces(&raw, 16))
        let count = min(max(n, 0), 16)
        func cstr<T>(_ tuple: T) -> String {
            withUnsafeBytes(of: tuple) { ptr in
                let bytes = ptr.prefix { $0 != 0 }
                return String(decoding: bytes, as: UTF8.self)
            }
        }
        let tags = ["Other", "Ethernet", "Wi-Fi", "USB-LAN", "Virtual"]
        networkInterfaces = (0..<count).map { i in
            let item = raw[i]
            let tag = Int(item.type) < tags.count ? tags[Int(item.type)] : "Other"
            return NetInterfaceOption(
                id: cstr(item.id),
                systemName: cstr(item.system_name),
                friendlyName: cstr(item.friendly_name),
                typeTag: tag,
                ipv4: cstr(item.ipv4_address),
                speedMbps: item.link_speed_mbps,
                isUp: item.is_up != 0
            )
        }
    }

    private func pushMix(_ s: ChannelStrip) {
        guard let h = cli else { return }
        lv_monitor_client_set_channel_mix(h, Int32(s.id), s.gainDb, s.pan, s.mute ? 1 : 0, s.solo ? 1 : 0)
    }

    private func pushDsp() {
        guard let h = cli else { return }
        var p = dsp.cParams
        lv_monitor_client_set_dsp(h, &p)
    }

    private func pushStream() {
        guard let h = cli else { return }
        lv_monitor_client_set_stream(h, quality.wireKbps, UInt16(buffer.rawValue))
    }

    // MARK: - Network loop

    private final class IoState: @unchecked Sendable {
        private let lock = NSLock()
        private var _running = false
        private var _stopped = true
        var running: Bool {
            get { lock.lock(); defer { lock.unlock() }; return _running }
            set { lock.lock(); _running = newValue; lock.unlock() }
        }
        func markStopped(_ v: Bool) { lock.lock(); _stopped = v; lock.unlock() }
        func waitStopped() {
            for _ in 0..<500 {  // <= 0.5 s
                lock.lock(); let s = _stopped; lock.unlock()
                if s { return }
                usleep(1000)
            }
        }
    }

    private func startIo(_ h: OpaquePointer) {
        io.running = true
        io.markStopped(false)
        let state = io
        let handle = UInt(bitPattern: h)
        let t = Thread { [weak self] in
            let cli = OpaquePointer(bitPattern: handle)!
            var lastMeta = Date.distantPast, lastKeep = Date()
            var names = [LV_MonitorMetaChannel](repeating: LV_MonitorMetaChannel(), count: 64)
            while state.running {
                lv_monitor_client_poll(cli)
                let now = Date()
                if now.timeIntervalSince(lastKeep) > 1.0 { lv_monitor_client_keepalive(cli); lastKeep = now }
                if now.timeIntervalSince(lastMeta) > 1.0 / 15.0 {
                    lv_monitor_client_request_meta(cli)
                    lastMeta = now
                    var tel = LV_MonitorTelemetry()
                    var dtel = LV_MonitorDanteTelemetry()
                    let n = Int(lv_monitor_client_get_meta(cli, &names, 64, &tel))
                    lv_monitor_client_get_dante_telemetry(cli, &dtel)
                    let snapshot: [(String, Float)] = (0..<min(max(n, 0), 64)).map { i in
                        let name = withUnsafeBytes(of: names[i].name) { raw -> String in
                            let bytes = raw.prefix { $0 != 0 }
                            return String(decoding: bytes, as: UTF8.self)
                        }
                        return (name, Float(names[i].peak_dbfs))
                    }
                    let playoutMs = dtel.effective_playout_ms > 0 ? dtel.effective_playout_ms : tel.buffer_ms
                    let t = MonitorTelemetry(gainReductionDb: tel.bus_gain_reduction_db,
                                             mixPeakDbfs: tel.bus_peak_dbfs, bufferMs: tel.buffer_ms,
                                             audioPackets: tel.audio_packets_rx, receiving: false,
                                             clockLocked: dtel.clock_locked != 0,
                                             clockOffsetJitterUs: dtel.clock_offset_jitter_us,
                                             duplicatesRecovered: dtel.duplicates_recovered,
                                             activeFlowChannels: dtel.active_flow_channels,
                                             effectivePlayoutMs: playoutMs,
                                             wifiRedundancyEnabled: dtel.wifi_redundancy_enabled != 0)
                    Task { @MainActor in self?.applyMeta(snapshot, t) }
                }
                usleep(1000)
            }
            state.markStopped(true)
        }
        t.qualityOfService = .userInteractive
        t.threadPriority = 1.0
        t.name = "LanVidMonitor.io"
        ioThread = t
        t.start()
    }

    private var lastPackets: UInt64 = 0
    private var connectStartedAt: Date = .distantPast
    private var handshakeConfirmed = false

    private func applyMeta(_ snap: [(String, Float)], _ t: MonitorTelemetry) {
        guard cli != nil else { return }
        let saved = loadMix()
        var next: [ChannelStrip] = []
        for (i, item) in snap.enumerated() {
            if i < channels.count {
                var s = channels[i]
                s.name = item.0.isEmpty ? "Ch \(i + 1)" : item.0
                s.peakDbfs = item.1
                next.append(s)
            } else {  // a channel appeared on the PC: restore this phone's last mix for it
                var s = saved[i] ?? ChannelStrip(id: i, name: "")
                s.name = item.0.isEmpty ? "Ch \(i + 1)" : item.0
                s.peakDbfs = item.1
                next.append(s)
                pushMix(s)
            }
        }
        let prevCount = channels.count
        if next != channels { channels = next }
        if next.count != prevCount && !next.isEmpty { pushSubscribedMask() }
        if !next.isEmpty || t.audioPackets > 0 {
            if !handshakeConfirmed, !isDemo, let s = connected {
                // The studio really answered: remember it. The LAN scan probes this address first next launch.
                UserDefaults.standard.set(s.host, forKey: "lastHost")
                UserDefaults.standard.set(Int(s.port), forKey: "lastPort")
            }
            handshakeConfirmed = true
            if lastError != nil { lastError = nil }
        } else if !handshakeConfirmed && Date().timeIntervalSince(connectStartedAt) > 3.5, let s = connected {
            lastError = tr("Немає відповіді від \(s.host):\(s.port) — перевірте Wi-Fi, LanVid Studio на ПК та Firewall (UDP 5020)",
                           "No response from \(s.host):\(s.port) — check Wi-Fi, LanVid Studio on PC, and Firewall (UDP 5020)")
        }
        var tt = t
        tt.receiving = t.audioPackets > lastPackets
        lastPackets = t.audioPackets
        telemetry = tt
    }

    // MARK: - Low-latency playback (AVAudioEngine, 5 ms IO buffer)

    private func startAudio(_ h: OpaquePointer) {
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playback, mode: .default, options: [])
            try session.setPreferredSampleRate(48000)
            try session.setPreferredIOBufferDuration(0.005)  // 240 frames @ 48 kHz
            try session.setActive(true)
        } catch {
            lastError = tr("Аудіосесія: ", "Audio session: ") + error.localizedDescription
        }
        let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
        let scratch = self.scratch
        let cap = scratchFrames
        let handle = UInt(bitPattern: h)
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, abl -> OSStatus in
            let n = min(Int(frameCount), cap)
            guard let cli = OpaquePointer(bitPattern: handle) else { return noErr }
            lv_monitor_client_pop_stereo(cli, scratch, UInt32(n))  // interleaved L R L R ...
            let bufs = UnsafeMutableAudioBufferListPointer(abl)
            if bufs.count >= 2, let l = bufs[0].mData?.assumingMemoryBound(to: Float.self),
               let r = bufs[1].mData?.assumingMemoryBound(to: Float.self) {
                for i in 0..<n { l[i] = scratch[2 * i]; r[i] = scratch[2 * i + 1] }
            }
            return noErr
        }
        sourceNode = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)  // engine converts to the HW rate
        engine.prepare()
        do {
            try engine.start()
        } catch {
            lastError = tr("Аудіо: ", "Audio: ") + error.localizedDescription
        }
        audioStatus = String(format: tr("%.0f кГц · буфер %.1f мс", "%.0f kHz · buffer %.1f ms"),
                             session.sampleRate / 1000, session.ioBufferDuration * 1000)
    }

    private func stopAudio() {
        engine.stop()
        if let n = sourceNode {
            engine.detach(n)
            sourceNode = nil
        }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        audioStatus = ""
    }

    private func restartAudio() {
        guard let h = cli else { return }
        stopAudio()
        startAudio(h)
    }

    private func handleInterruption(_ n: Notification) {
        guard let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: raw) else { return }
        if type == .ended { restartAudio() }
    }

    // MARK: - Persistence

    private func save() {
        let d = UserDefaults.standard
        d.set(quality.rawValue, forKey: "quality")
        d.set(buffer.rawValue, forKey: "buffer")
        d.set(wifiRedundancy, forKey: "wifiRedundancy")
        d.set(wifiRedundancyUserSet, forKey: "wifiRedundancyUserSet")
        d.set(primaryNicIp, forKey: "primaryNicIp")
        d.set(secondaryNicIp, forKey: "secondaryNicIp")
        d.set(clientName, forKey: "clientName")
        if let data = try? JSONEncoder().encode(dsp) { d.set(data, forKey: "dsp") }
    }

    private func load() {
        let d = UserDefaults.standard
        if let q = StreamQuality(rawValue: d.object(forKey: "quality") as? Int ?? 256) { quality = q }
        if let b = MonitorBuffer(rawValue: d.object(forKey: "buffer") as? Int ?? 10) { buffer = b }
        wifiRedundancyUserSet = d.bool(forKey: "wifiRedundancyUserSet")
        if wifiRedundancyUserSet {
            wifiRedundancy = d.bool(forKey: "wifiRedundancy")
        } else {
            wifiRedundancy = (quality == .k192 || quality == .k256)
        }
        if let p = d.string(forKey: "primaryNicIp") { primaryNicIp = p }
        if let s = d.string(forKey: "secondaryNicIp") { secondaryNicIp = s }
        if let n = d.string(forKey: "clientName"), !n.isEmpty { clientName = n }
        if let data = d.data(forKey: "dsp"), let v = try? JSONDecoder().decode(DspSettings.self, from: data) { dsp = v }
    }

    private struct SavedStrip: Codable { var gainDb: Float; var pan: Float; var mute: Bool; var solo: Bool }

    private func saveMix() {
        if isDemo { return }  // the simulated studio never overwrites the real mix
        var m: [String: SavedStrip] = [:]
        for c in channels { m[String(c.id)] = SavedStrip(gainDb: c.gainDb, pan: c.pan, mute: c.mute, solo: c.solo) }
        if let data = try? JSONEncoder().encode(m) { UserDefaults.standard.set(data, forKey: "mix") }
    }

    private func loadMix() -> [Int: ChannelStrip] {
        guard let data = UserDefaults.standard.data(forKey: "mix"),
              let m = try? JSONDecoder().decode([String: SavedStrip].self, from: data) else { return [:] }
        var out: [Int: ChannelStrip] = [:]
        for (k, v) in m {
            if let i = Int(k) {
                out[i] = ChannelStrip(id: i, name: "", gainDb: v.gainDb, pan: v.pan, mute: v.mute, solo: v.solo)
            }
        }
        return out
    }

    var lastManualHost: String { UserDefaults.standard.string(forKey: "lastHost") ?? "" }
    var lastManualPort: UInt16 {
        let p = UserDefaults.standard.integer(forKey: "lastPort")
        return p > 0 ? UInt16(p) : 5020
    }
}

/// Default device name for kHello (shown in LanVid Studio's phone list; max 15 bytes on the wire). Generic on
/// purpose: the host name can contain the owner's real name; the user can change it in Settings.
enum UIDeviceName {
    static var current: String { "iPhone" }
}
