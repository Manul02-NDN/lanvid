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

    @Published var quality: StreamQuality = .k256 { didSet { if quality != oldValue { pushStream(); save() } } }
    @Published var buffer: MonitorBuffer = .ms10 { didSet { if buffer != oldValue { pushStream(); save() } } }
    @Published var dsp = DspSettings() { didSet { if dsp != oldValue { pushDsp(); save() } } }
    @Published var clientName: String = UIDeviceName.current { didSet { save() } }

    private var cli: OpaquePointer?
    private var ioThread: Thread?
    private let io = IoState()
    private let engine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?
    private let scratchFrames = 8192
    private let scratch: UnsafeMutablePointer<Float>
    private var observers: [NSObjectProtocol] = []

    init() {
        scratch = .allocate(capacity: 8192 * 2)
        scratch.initialize(repeating: 0, count: 8192 * 2)
        load()
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
        disconnect()
        guard let h = lv_monitor_client_create(48000) else { lastError = "Не вдалося створити клієнт"; return }
        let rc = lv_monitor_client_connect(h, studio.host, studio.port, String(clientName.prefix(15)),
                                           quality.wireKbps, UInt16(buffer.rawValue))
        guard rc == 0 else {
            lv_monitor_client_destroy(h)
            lastError = "Немає з'єднання з \(studio.host):\(studio.port)"
            return
        }
        cli = h
        connected = studio
        lastError = nil
        UserDefaults.standard.set(studio.host, forKey: "lastHost")
        UserDefaults.standard.set(Int(studio.port), forKey: "lastPort")
        pushDsp()
        for ch in channels { pushMix(ch) }
        startIo(h)
        startAudio(h)
    }

    func disconnect() {
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

    // MARK: - Mixer (sent instantly as kMonitorControl; the PC re-mixes the phone's stereo bus)

    func update(_ strip: ChannelStrip) {
        guard let i = channels.firstIndex(where: { $0.id == strip.id }) else { return }
        var s = strip
        s.peakDbfs = channels[i].peakDbfs
        guard channels[i] != s else { return }
        channels[i] = s
        pushMix(s)
        saveMix()
    }

    func resetMix() {
        for i in channels.indices {
            channels[i].gainDb = 0; channels[i].pan = 0; channels[i].mute = false; channels[i].solo = false
            pushMix(channels[i])
        }
        saveMix()
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
                    let n = Int(lv_monitor_client_get_meta(cli, &names, 64, &tel))
                    let snapshot: [(String, Float)] = (0..<min(max(n, 0), 64)).map { i in
                        let name = withUnsafeBytes(of: names[i].name) { raw -> String in
                            let bytes = raw.prefix { $0 != 0 }
                            return String(decoding: bytes, as: UTF8.self)
                        }
                        return (name, Float(names[i].peak_dbfs))
                    }
                    let t = MonitorTelemetry(gainReductionDb: tel.bus_gain_reduction_db,
                                             mixPeakDbfs: tel.bus_peak_dbfs, bufferMs: tel.buffer_ms,
                                             audioPackets: tel.audio_packets_rx, receiving: false)
                    Task { @MainActor in self?.applyMeta(snapshot, t) }
                }
                usleep(1000)
            }
            state.markStopped(true)
        }
        t.qualityOfService = .userInteractive
        t.name = "LanVidMonitor.io"
        ioThread = t
        t.start()
    }

    private var lastPackets: UInt64 = 0

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
        if next != channels { channels = next }
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
            lastError = "Аудіосесія: \(error.localizedDescription)"
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
            lastError = "Аудіо: \(error.localizedDescription)"
        }
        audioStatus = String(format: "%.0f кГц · буфер %.1f мс", session.sampleRate / 1000,
                             session.ioBufferDuration * 1000)
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
        d.set(clientName, forKey: "clientName")
        if let data = try? JSONEncoder().encode(dsp) { d.set(data, forKey: "dsp") }
    }

    private func load() {
        let d = UserDefaults.standard
        if let q = StreamQuality(rawValue: d.object(forKey: "quality") as? Int ?? 256) { quality = q }
        if let b = MonitorBuffer(rawValue: d.object(forKey: "buffer") as? Int ?? 10) { buffer = b }
        if let n = d.string(forKey: "clientName"), !n.isEmpty { clientName = n }
        if let data = d.data(forKey: "dsp"), let v = try? JSONDecoder().decode(DspSettings.self, from: data) { dsp = v }
    }

    private struct SavedStrip: Codable { var gainDb: Float; var pan: Float; var mute: Bool; var solo: Bool }

    private func saveMix() {
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

/// Device name for kHello (shown in LanVid Studio's phone list; max 15 bytes on the wire).
enum UIDeviceName {
    static var current: String {
        #if canImport(UIKit)
        return String(ProcessInfo.processInfo.hostName.split(separator: ".").first ?? "iPhone")
        #else
        return "iPhone"
        #endif
    }
}
