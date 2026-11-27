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

    private let cbLock = NSLock()
    private var handle: OpaquePointer?
    private var pool: CVPixelBufferPool?
    private var poolWidth: Int32 = 0
    private var poolHeight: Int32 = 0

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?

    private var _onVideoPixelBuffer: ((CVPixelBuffer, UInt32) -> Void)?
    public var onVideoPixelBuffer: ((CVPixelBuffer, UInt32) -> Void)? {
        get { cbLock.lock(); defer { cbLock.unlock() }; return _onVideoPixelBuffer }
        set { cbLock.lock(); defer { cbLock.unlock() }; _onVideoPixelBuffer = newValue }
    }

    private var _onStateChanged: ((Int32, Int32, String) -> Void)?
    public var onStateChanged: ((Int32, Int32, String) -> Void)? {
        get { cbLock.lock(); defer { cbLock.unlock() }; return _onStateChanged }
        set { cbLock.lock(); defer { cbLock.unlock() }; _onStateChanged = newValue }
    }

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
            lv_receiver_set_video_callback(h, nil, nil)
            lv_receiver_set_state_callback(h, nil, nil)
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
            lv_receiver_set_video_callback(h, nil, nil)
            lv_receiver_set_state_callback(h, nil, nil)
        }
    }

    public var localPort: UInt16 {
        handle.map { lv_receiver_get_local_port($0) } ?? 0
    }

    public func setVolume(_ volume: Float) {
        if let h = handle { lv_receiver_set_volume(h, volume) }
    }

    /// Adjust stereo audio jitter buffer target delay live (5...60 ms).
    public func setAudioTargetMs(_ targetMs: Double) {
        if let h = handle { _ = lv_receiver_set_audio_target_ms(h, targetMs) }
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
            let rowBytes = min(Int(frame.width) * 4, min(dstStride, srcStride))
            if rowBytes > 0 {
                if dstStride == srcStride && dstStride == rowBytes {
                    memcpy(dstBase, srcData, rowBytes * Int(frame.height))
                } else {
                    for y in 0..<Int(frame.height) {
                        memcpy(dstBase.advanced(by: y * dstStride), srcData.advanced(by: y * srcStride), rowBytes)
                    }
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
        // Preallocate scratch PCM16 buffer once outside the real-time audio render block (zero heap allocs in callback)
        let chunkCap = 4096
        let scratchPtr = UnsafeMutablePointer<Int16>.allocate(capacity: chunkCap * 2)
        scratchPtr.initialize(repeating: 0, count: chunkCap * 2)
        let scratchHolder = ScratchBufferBox(ptr: scratchPtr, count: chunkCap * 2)

        let node = AVAudioSourceNode(format: format) { _, _, frameCount, audioBufferList -> OSStatus in
            let total = Int(frameCount)
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard abl.count >= 2,
                  let chL = abl[0].mData?.assumingMemoryBound(to: Float.self),
                  let chR = abl[1].mData?.assumingMemoryBound(to: Float.self) else {
                return noErr
            }
            let scale: Float = 1.0 / 32768.0
            let buf = scratchHolder.ptr
            var offset = 0
            while offset < total {
                let n = min(chunkCap, total - offset)
                _ = lv_receiver_pop_audio_pcm16(h, buf, Int32(n))
                for i in 0..<n {
                    chL[offset + i] = Float(buf[i * 2]) * scale
                    chR[offset + i] = Float(buf[i * 2 + 1]) * scale
                }
                offset += n
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

    private final class ScratchBufferBox {
        let ptr: UnsafeMutablePointer<Int16>
        let count: Int
        init(ptr: UnsafeMutablePointer<Int16>, count: Int) {
            self.ptr = ptr
            self.count = count
        }
        deinit {
            ptr.deinitialize(count: count)
            ptr.deallocate()
        }
    }
}
