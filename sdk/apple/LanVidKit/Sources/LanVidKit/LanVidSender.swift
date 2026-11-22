import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import LanVidCore

/// Idiomatic Swift Sender wrapping `LvSenderHandle` with automatic memory cleanup (`deinit`)
/// and zero-copy `CVPixelBuffer` / `CMSampleBuffer` submission (AVFoundation & ReplayKit).
public final class LanVidSender {
    public struct Configuration: Sendable {
        public var destHost: String
        public var destPort: UInt16
        public var quality: Int32
        public var fps: Double
        public var fecGroupSize: Int32
        public var adaptiveQuality: Bool
        public var qMin: Int32
        public var qMax: Int32
        public var codec: LanVidCodec
        public var bitrateKbps: UInt32
        public var keyframeInterval: UInt32

        public init(
            destHost: String,
            destPort: UInt16 = 5000,
            quality: Int32 = 80,
            fps: Double = 60.0,
            fecGroupSize: Int32 = 6,
            adaptiveQuality: Bool = true,
            qMin: Int32 = 35,
            qMax: Int32 = 85,
            codec: LanVidCodec = .dct,
            bitrateKbps: UInt32 = 8000,
            keyframeInterval: UInt32 = 120
        ) {
            self.destHost = destHost
            self.destPort = destPort
            self.quality = quality
            self.fps = fps
            self.fecGroupSize = fecGroupSize
            self.adaptiveQuality = adaptiveQuality
            self.qMin = qMin
            self.qMax = qMax
            self.codec = codec
            self.bitrateKbps = bitrateKbps
            self.keyframeInterval = keyframeInterval
        }
    }

    private var handle: OpaquePointer?

    public init() throws {
        try LanVidRuntime.ensureInitialized()
        guard let h = lv_sender_create() else {
            throw LanVidError.coreError(code: -8, message: "lv_sender_create returned nil")
        }
        self.handle = h
    }

    deinit {
        if let h = handle {
            lv_sender_stop(h)
            lv_sender_destroy(h)
        }
    }

    public func start(config: Configuration) throws {
        guard let h = handle else { return }
        var c = LV_SenderConfig()
        lv_sdk_sender_config_default(&c)
        writeCString(config.destHost, into: &c.dest_ip)
        c.dest_port = config.destPort
        c.quality = config.quality
        c.fps = config.fps
        c.fec_group_size = config.fecGroupSize
        c.adaptive_quality = config.adaptiveQuality ? 1 : 0
        c.q_min = config.qMin
        c.q_max = config.qMax
        c.codec = config.codec.rawValue
        c.bitrate_kbps = config.bitrateKbps
        c.keyframe_interval = config.keyframeInterval

        let rc = lv_sdk_sender_start(h, &c)
        if rc != LV_OK.rawValue {
            throw LanVidError.coreError(code: rc, message: String(cString: lv_error_string(rc)))
        }
    }

    public func stop() {
        guard let h = handle else { return }
        lv_sender_stop(h)
    }

    /// Zero-copy push of a `CMSampleBuffer` straight from `AVCaptureVideoDataOutput` or `ReplayKit`.
    public func push(sampleBuffer: CMSampleBuffer) throws {
        guard let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        try push(pixelBuffer: pb)
    }

    /// Zero-copy push of a `CVPixelBuffer` (supports 32BGRA and NV12 bi-planar 4:2:0 without allocations).
    public func push(pixelBuffer: CVPixelBuffer, timestampUs: UInt32 = 0) throws {
        guard let h = handle else { return }
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

        let width = Int32(CVPixelBufferGetWidth(pixelBuffer))
        let height = Int32(CVPixelBufferGetHeight(pixelBuffer))
        let fmt = CVPixelBufferGetPixelFormatType(pixelBuffer)

        let rc: Int32
        if fmt == kCVPixelFormatType_32BGRA {
            guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
            let stride = Int32(CVPixelBufferGetBytesPerRow(pixelBuffer))
            rc = lv_sender_push_cvpixelbuffer_planes(
                h,
                base.assumingMemoryBound(to: UInt8.self),
                stride,
                nil,
                0,
                width,
                height,
                LV_PIXEL_FORMAT_BGRA8888.rawValue,
                timestampUs
            )
        } else if fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                  fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange {
            guard let p0 = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0),
                  let p1 = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1) else { return }
            let s0 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0))
            let s1 = Int32(CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1))
            rc = lv_sender_push_cvpixelbuffer_planes(
                h,
                p0.assumingMemoryBound(to: UInt8.self),
                s0,
                p1.assumingMemoryBound(to: UInt8.self),
                s1,
                width,
                height,
                LV_PIXEL_FORMAT_NV12.rawValue,
                timestampUs
            )
        } else {
            throw LanVidError.unsupportedPixelFormat(fmt)
        }

        if rc != LV_OK.rawValue {
            throw LanVidError.coreError(code: rc, message: String(cString: lv_error_string(rc)))
        }
    }

    /// Push interleaved PCM16 or Float32 audio frames (e.g. from AVAudioEngine / ReplayKit).
    public func pushAudioPCM16(_ samples: UnsafePointer<Int16>, frameCount: Int32,
                               sampleRate: Int32 = 48000, channels: Int32 = 2,
                               timestampUs: UInt32 = 0) {
        guard let h = handle else { return }
        _ = lv_sender_push_audio_chunk(
            h, samples, frameCount, sampleRate, channels, LV_AUDIO_FORMAT_PCM16.rawValue, timestampUs
        )
    }

    public func requestQuality(_ quality: Int32) {
        guard let h = handle else { return }
        _ = lv_sender_request_keyframe_quality(h, quality)
    }

    public func setBitrateKbps(_ bitrateKbps: UInt32) {
        guard let h = handle else { return }
        _ = lv_sender_set_bitrate(h, bitrateKbps)
    }

    public func forceKeyframe() {
        guard let h = handle else { return }
        _ = lv_sender_force_keyframe(h)
    }

    public func setFecGroupSize(_ groupSize: Int32) {
        guard let h = handle else { return }
        _ = lv_sender_set_fec_group(h, groupSize)
    }

    public func setAdaptiveQuality(enabled: Bool, qMin: Int32 = 35, qMax: Int32 = 85) {
        guard let h = handle else { return }
        _ = lv_sender_set_adaptive_quality(h, enabled ? 1 : 0, qMin, qMax)
    }

    public var stats: LanVidStats {
        var s = LV_StreamStats()
        if let h = handle { lv_sender_get_stream_stats(h, &s) }
        return LanVidStats(c: s)
    }
}
