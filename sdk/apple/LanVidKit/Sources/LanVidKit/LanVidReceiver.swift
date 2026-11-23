import AVFoundation
import CoreVideo
import Foundation
import LanVidCore

/// Idiomatic Swift Receiver delivering decoded frames directly as Metal-ready `CVPixelBuffer`s
/// (`AsyncStream<CVPixelBuffer>` or callback) with built-in low-latency `AVAudioEngine` playback.
public final class LanVidReceiver {
    public struct Configuration: Sendable {
        public var bindPort: UInt16
        public var deviceName: String
        public var audioTargetMs: Double
        public var enableAudioPlayback: Bool
        public var enableDiscovery: Bool

        public init(
            bindPort: UInt16 = 5000,
            deviceName: String? = nil,
            audioTargetMs: Double = 12.0,
            enableAudioPlayback: Bool = true,
            enableDiscovery: Bool = true
        ) {
            self.bindPort = bindPort
            if let name = deviceName {
                self.deviceName = name
            } else {
                #if os(macOS)
                self.deviceName = Host.current().localizedName ?? "MacDevice"
                #else
                self.deviceName = "iOSDevice"
                #endif
            }
            self.audioTargetMs = audioTargetMs
            self.enableAudioPlayback = enableAudioPlayback
            self.enableDiscovery = enableDiscovery
        }
    }

    private var handle: OpaquePointer?
    private var pool: CVPixelBufferPool?
    private var poolWidth: Int32 = 0
    private var poolHeight: Int32 = 0

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?

    public var onVideoPixelBuffer: ((CVPixelBuffer, UInt32) -> Void)?
    public var onStateChanged: ((Int32, Int32, String) -> Void)?

    public init() throws {
        try LanVidRuntime.ensureInitialized()
        guard let h = lv_receiver_create() else {
            throw LanVidError.coreError(code: -8, message: "lv_receiver_create returned nil")
        }
        self.handle = h
    }

    deinit {
        stop()
        if let h = handle {
            lv_receiver_set_video_callback(h, nil, nil)
            lv_receiver_set_state_callback(h, nil, nil)
            lv_receiver_destroy(h)
        }
    }

    public func start(config: Configuration = Configuration()) throws {
        guard let h = handle else { return }

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        lv_receiver_set_video_callback(h, { framePtr, ud in
            guard let frame = framePtr?.pointee, let ud else { return }
            let receiver = Unmanaged<LanVidReceiver>.fromOpaque(ud).takeUnretainedValue()
            receiver.handleDecodedFrame(frame)
        }, selfPtr)

        lv_receiver_set_state_callback(h, { state, err, msg, ud in
            guard let ud else { return }
            let receiver = Unmanaged<LanVidReceiver>.fromOpaque(ud).takeUnretainedValue()
            let text = msg.map { String(cString: $0) } ?? ""
            receiver.onStateChanged?(state, err, text)
        }, selfPtr)

        var c = LV_ReceiverConfig()
        lv_sdk_receiver_config_default(&c)
        c.bind_port = config.bindPort
        c.output_format = Int32(LV_PIXEL_FORMAT_BGRA8888.rawValue)
        c.audio_target_ms = config.audioTargetMs
        c.enable_os_audio = 0 // AVAudioEngine pulls directly from lock-free jitter buffer
        c.enable_discovery = config.enableDiscovery ? 1 : 0
        writeCString(config.deviceName, into: &c.device_name)

        let rc = lv_sdk_receiver_start(h, &c)
        if rc != LV_OK.rawValue {
            throw LanVidError.coreError(code: rc, message: String(cString: lv_error_string(rc)))
        }
        if config.enableAudioPlayback {
            startAudioEngine()
        }
    }

    public func stop() {
        stopAudioEngine()
        if let h = handle {
            lv_receiver_stop(h)
        }
    }

    public var localPort: UInt16 {
        handle.map { lv_receiver_get_local_port($0) } ?? 0
    }

    public func setVolume(_ volume: Float) {
        if let h = handle { lv_receiver_set_volume(h, volume) }
    }

    // MARK: - 64-channel phase-coherent bus

    public func configureAudioBus(latencyMs: UInt32 = 20, channels: Int32 = 64, blockFrames: UInt32 = 240,
                                  sampleRate: Int32 = 48000) throws {
        guard let h = handle else { return }
        var bus = LV_AudioBusConfig(sample_rate: sampleRate, latency_ms: latencyMs,
                                    num_channels: channels, block_frames: blockFrames)
        let rc = lv_receiver_configure_audio_bus(h, &bus)
        if rc != LV_OK.rawValue {
            throw LanVidError.coreError(code: rc, message: String(cString: lv_error_string(rc)))
        }
    }

    /// Live latency change (2...2000 ms) without reconnecting.
    public func setAudioLatencyMs(_ ms: UInt32) {
        guard let h = handle else { return }
        _ = lv_receiver_set_audio_latency_ms(h, ms)
    }

    /// Latency mode, live: `true` = Adaptive Wi-Fi (default), `false` = Fixed (Ethernet / live monitoring).
    public func setAdaptiveJitter(_ enabled: Bool) {
        guard let h = handle else { return }
        _ = lv_receiver_set_adaptive_jitter(h, enabled ? 1 : 0)
    }

    /// Measured jitter-buffer playout delay in milliseconds (0 before playout starts).
    public var effectiveLatencyMs: Double {
        guard let h = handle else { return 0 }
        return lv_receiver_get_effective_latency_ms(h)
    }

    /// Pull sample-aligned Float32 frames for channels 0..<channels (call from an AVAudioSourceNode
    /// render block). `planar == true` writes channel-major (`out[ch * frames + i]`).
    @discardableResult
    public func popAudioMultichannel(into out: UnsafeMutablePointer<Float>, channels: UInt16, frames: UInt32,
                                     planar: Bool = false) -> Int32 {
        guard let h = handle else { return Int32(LV_ERR_NOT_RUNNING.rawValue) }
        return lv_receiver_pop_audio_multichannel(h, out, channels, frames,
                                                  planar ? LV_AUDIO_LAYOUT_PLANAR : LV_AUDIO_LAYOUT_INTERLEAVED)
    }

    public func channelStats(_ channel: UInt16) -> LV_AudioChannelStats {
        var st = LV_AudioChannelStats()
        if let h = handle { _ = lv_receiver_get_channel_stats(h, channel, &st) }
        return st
    }

    public var stats: LanVidStats {
        var s = LV_StreamStats()
        if let h = handle { lv_receiver_get_stream_stats(h, &s) }
        return LanVidStats(c: s)
    }

    /// AsyncStream interface yielding Metal-compatible `CVPixelBuffer` frames.
    public func videoFrames() -> AsyncStream<CVPixelBuffer> {
        AsyncStream(bufferingPolicy: .bufferingNewest(2)) { continuation in
            self.onVideoPixelBuffer = { pb, _ in
                continuation.yield(pb)
            }
            continuation.onTermination = { [weak self] _ in
                self?.onVideoPixelBuffer = nil
            }
        }
    }

    private func handleDecodedFrame(_ frame: LV_VideoFrameView) {
        guard let srcData = frame.data, frame.width > 0, frame.height > 0 else { return }
        ensurePool(width: frame.width, height: frame.height)
        guard let pool else { return }

        var pbOut: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pbOut) == kCVReturnSuccess,
              let pb = pbOut else { return }

        CVPixelBufferLockBaseAddress(pb, [])
        if let dstBase = CVPixelBufferGetBaseAddress(pb) {
            let dstStride = CVPixelBufferGetBytesPerRow(pb)
            let srcStride = Int(frame.stride)
            let rowBytes = Int(frame.width) * 4
            if dstStride == srcStride {
                memcpy(dstBase, srcData, rowBytes * Int(frame.height))
            } else {
                for y in 0..<Int(frame.height) {
                    memcpy(dstBase.advanced(by: y * dstStride), srcData.advanced(by: y * srcStride), rowBytes)
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        onVideoPixelBuffer?(pb, frame.frame_id)
    }

    private func ensurePool(width: Int32, height: Int32) {
        if pool != nil && poolWidth == width && poolHeight == height { return }
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey: Int(width),
            kCVPixelBufferHeightKey: Int(height),
            kCVPixelBufferMetalCompatibilityKey: true,
            kCVPixelBufferIOSurfacePropertiesKey: [:]
        ]
        var newPool: CVPixelBufferPool?
        if CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs as CFDictionary, &newPool) == kCVReturnSuccess {
            pool = newPool
            poolWidth = width
            poolHeight = height
        }
    }

    private func startAudioEngine() {
        guard audioEngine == nil, let h = handle else { return }
        let engine = AVAudioEngine()
        guard let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 48000, channels: 2, interleaved: false) else {
            return
        }
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList -> OSStatus in
            let n = Int(frameCount)
            var tmp = [Int16](repeating: 0, count: n * 2)
            _ = lv_receiver_pop_audio_pcm16(h, &tmp, Int32(n))
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            if abl.count >= 2,
               let chL = abl[0].mData?.assumingMemoryBound(to: Float.self),
               let chR = abl[1].mData?.assumingMemoryBound(to: Float.self) {
                let scale: Float = 1.0 / 32768.0
                for i in 0..<n {
                    chL[i] = Float(tmp[i * 2]) * scale
                    chR[i] = Float(tmp[i * 2 + 1]) * scale
                }
            }
            return noErr
        }
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        try? engine.start()
        self.audioEngine = engine
        self.sourceNode = node
    }

    private func stopAudioEngine() {
        audioEngine?.stop()
        audioEngine = nil
        sourceNode = nil
    }
}
