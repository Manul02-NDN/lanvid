import CrossLinkCore
import Foundation

/// Idiomatic Swift 64-channel AoIP + MIDI 2.0 Sender wrapping `ClSenderHandle`.
public final class CrossLinkSender {
    public struct Configuration: Sendable {
        public var destHost: String
        public var destPort: UInt16
        public var sampleRate: Int32
        public var latencyMs: UInt32
        public var audioCodec: Int32

        public init(
            destHost: String,
            destPort: UInt16 = 5004,
            sampleRate: Int32 = 48000,
            latencyMs: UInt32 = 20,
            audioCodec: Int32 = 0
        ) {
            self.destHost = destHost
            self.destPort = destPort
            self.sampleRate = sampleRate
            self.latencyMs = latencyMs
            self.audioCodec = audioCodec
        }
    }

    private var handle: OpaquePointer?

    public init() throws {
        try CrossLinkRuntime.ensureInitialized()
        guard let h = cl_sender_create() else {
            throw CrossLinkError.coreError(code: -8, message: "cl_sender_create returned nil")
        }
        self.handle = h
    }

    deinit {
        if let h = handle {
            cl_sender_stop(h)
            cl_sender_destroy(h)
        }
    }

    public func configureAudioBus(latencyMs: UInt32 = 20, sampleRate: Int32 = 48000) throws {
        guard let h = handle else { return }
        var bus = CL_AudioBusConfig()
        cl_audio_bus_config_default(&bus)
        bus.latency_ms = latencyMs
        bus.sample_rate = sampleRate
        let rc = cl_sender_configure_audio_bus(h, &bus)
        if rc != CL_OK.rawValue {
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func start(config: Configuration) throws {
        guard let h = handle else { return }
        var c = CL_SenderConfig()
        cl_sender_config_default(&c)
        clWriteCString(config.destHost, into: &c.dest_ip)
        c.dest_port = config.destPort
        c.sample_rate = config.sampleRate
        c.latency_ms = config.latencyMs
        c.audio_codec = config.audioCodec
        let rc = cl_sender_start(h, &c)
        if rc != CL_OK.rawValue {
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func stop() {
        if let h = handle { cl_sender_stop(h) }
    }

    public func pushAudioChannel(_ channel: UInt16, samples: UnsafePointer<Float>, count: UInt32,
                                 mediaTimestamp: UInt64 = .max) throws {
        guard let h = handle else { return }
        let rc = cl_sender_push_audio_channel(h, channel, samples, count, CL_AUDIO_FORMAT_F32, mediaTimestamp)
        if rc != CL_OK.rawValue {
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func pushAudioMultichannel(interleaved: UnsafePointer<Float>, channels: UInt16, frames: UInt32,
                                      mediaTimestamp: UInt64 = .max) throws {
        guard let h = handle else { return }
        let rc = cl_sender_push_audio_multichannel(h, interleaved, channels, frames, CL_AUDIO_FORMAT_F32,
                                                   CL_AUDIO_LAYOUT_INTERLEAVED, mediaTimestamp)
        if rc != CL_OK.rawValue {
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func pushMidi(status: UInt8, data1: UInt8, data2: UInt8, port: UInt8 = 0,
                         mediaTimestampUs: UInt64 = 0) throws {
        guard let h = handle else { return }
        let rc = cl_sender_push_midi(h, status, data1, data2, port, mediaTimestampUs)
        if rc != CL_OK.rawValue {
            throw CrossLinkError.coreError(code: rc, message: String(cString: cl_error_string(rc)))
        }
    }

    public func setLatencyMs(_ ms: UInt32) {
        if let h = handle { _ = cl_sender_set_audio_latency_ms(h, ms) }
    }

    public func setWifiRedundancy(enabled: Bool, staggerUs: UInt32 = 1500) {
        if let h = handle { _ = cl_sender_set_wifi_redundancy(h, enabled ? 1 : 0, staggerUs) }
    }

    public var masterClockSamples: UInt64 {
        handle.map { cl_sender_get_master_clock_samples($0) } ?? 0
    }

    public var stats: CrossLinkStreamStats {
        var s = CL_StreamStats()
        if let h = handle { _ = cl_sender_get_stream_stats(h, &s) }
        return CrossLinkStreamStats(c: s)
    }
}
