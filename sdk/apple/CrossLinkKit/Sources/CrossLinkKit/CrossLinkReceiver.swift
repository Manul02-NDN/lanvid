import CrossLinkCore
import Foundation

/// Idiomatic Swift 64-channel phase-coherent AoIP + MIDI 2.0 Receiver wrapping `ClReceiverHandle`.
public final class CrossLinkReceiver {
    public struct Configuration: Sendable {
        public var bindPort: UInt16
        public var discoveryPort: UInt16
        public var sampleRate: Int32
        public var latencyMs: UInt32
        public var numChannels: Int32
        public var blockFrames: UInt32
        public var adaptiveJitter: Bool
        public var enableDiscovery: Bool
        public var deviceName: String

        public init(
            bindPort: UInt16 = 5004,
            discoveryPort: UInt16 = 5021,
            sampleRate: Int32 = 48000,
            latencyMs: UInt32 = 20,
            numChannels: Int32 = 64,
            blockFrames: UInt32 = 240,
            adaptiveJitter: Bool = true,
            enableDiscovery: Bool = true,
            deviceName: String? = nil
        ) {
            self.bindPort = bindPort
            self.discoveryPort = discoveryPort
            self.sampleRate = sampleRate
            self.latencyMs = latencyMs
            self.numChannels = numChannels
            self.blockFrames = blockFrames
            self.adaptiveJitter = adaptiveJitter
            self.enableDiscovery = enableDiscovery
            if let name = deviceName {
                self.deviceName = name
            } else {
                #if os(macOS)
                self.deviceName = Host.current().localizedName ?? "CrossLinkMac"
                #else
                self.deviceName = "CrossLinkiOS"
                #endif
            }
        }
    }

    private let cbLock = NSLock()
    private var handle: OpaquePointer?

    private var _onMidiEvent: ((CrossLinkMidiEvent) -> Void)?
    public var onMidiEvent: ((CrossLinkMidiEvent) -> Void)? {
        get { cbLock.lock(); defer { cbLock.unlock() }; return _onMidiEvent }
        set { cbLock.lock(); defer { cbLock.unlock() }; _onMidiEvent = newValue }
    }

    public init() throws {
        try CrossLinkRuntime.ensureInitialized()
        guard let h = cl_receiver_create() else {
            throw CrossLinkError.coreError(code: -8, message: "cl_receiver_create returned nil")
        }
        self.handle = h
    }

    deinit {
        stop()
        if let h = handle {
            cl_receiver_set_midi_callback(h, nil, nil)
            cl_receiver_destroy(h)
        }
    }

    public func start(config: Configuration = Configuration()) throws {
        guard let h = handle else { return }
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        cl_receiver_set_midi_callback(h, { evPtr, ud in
            guard let ev = evPtr?.pointee, let ud else { return }
            let receiver = Unmanaged<CrossLinkReceiver>.fromOpaque(ud).takeUnretainedValue()
            receiver.onMidiEvent?(CrossLinkMidiEvent(c: ev))
        }, selfPtr)

        var c = CL_ReceiverConfig()
        cl_receiver_config_default(&c)
        c.bind_port = config.bindPort
        c.discovery_port = config.discoveryPort
        c.sample_rate = config.sampleRate
        c.latency_ms = config.latencyMs
        c.num_channels = config.numChannels
        c.block_frames = config.blockFrames
        c.adaptive_jitter = config.adaptiveJitter ? 1 : 0
        c.enable_discovery = config.enableDiscovery ? 1 : 0
        clWriteCString(config.deviceName, into: &c.device_name)

        let rc = cl_receiver_start(h, &c)
        if rc != CL_OK.rawValue {
            cl_receiver_set_midi_callback(h, nil, nil)
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func stop() {
        if let h = handle {
            cl_receiver_stop(h)
            cl_receiver_set_midi_callback(h, nil, nil)
        }
    }

    public var localPort: UInt16 {
        handle.map { cl_receiver_get_local_port($0) } ?? 0
    }

    public func setLatencyMs(_ ms: UInt32) {
        if let h = handle { _ = cl_receiver_set_audio_latency_ms(h, ms) }
    }

    public func setAdaptiveJitter(_ enabled: Bool) {
        if let h = handle { _ = cl_receiver_set_adaptive_jitter(h, enabled ? 1 : 0) }
    }

    public var effectiveLatencyMs: Double {
        handle.map { cl_receiver_get_effective_latency_ms($0) } ?? 0.0
    }

    @discardableResult
    public func popAudioMultichannel(into out: UnsafeMutablePointer<Float>, channels: UInt16, frames: UInt32,
                                     planar: Bool = false) -> Int32 {
        guard let h = handle else { return Int32(CL_ERR_NOT_RUNNING.rawValue) }
        return cl_receiver_pop_audio_multichannel(h, out, channels, frames,
                                                  planar ? CL_AUDIO_LAYOUT_PLANAR : CL_AUDIO_LAYOUT_INTERLEAVED)
    }

    public func channelStats(_ channel: UInt16) -> CL_AudioChannelStats {
        var st = CL_AudioChannelStats()
        if let h = handle { _ = cl_receiver_get_channel_stats(h, channel, &st) }
        return st
    }

    public var stats: CrossLinkStreamStats {
        var s = CL_StreamStats()
        if let h = handle { _ = cl_receiver_get_stream_stats(h, &s) }
        return CrossLinkStreamStats(c: s)
    }
}
