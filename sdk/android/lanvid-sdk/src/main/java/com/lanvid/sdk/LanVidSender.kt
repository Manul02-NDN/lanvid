package com.lanvid.sdk

import android.media.Image
import com.lanvid.sdk.internal.NativeBridge
import org.json.JSONObject
import java.io.Closeable
import java.nio.ByteBuffer

class LanVidSender : Closeable {
    private var handle: Long = NativeBridge.senderCreate()

    fun start(
        destIp: String,
        destPort: Int = 5000,
        width: Int = 1280,
        height: Int = 720,
        fps: Int = 60,
        quality: Int = 80,
        fecGroup: Int = 6,
        adaptiveQuality: Boolean = true,
        codec: LanVidCodec = LanVidCodec.DCT,
        bitrateKbps: Int = 8000
    ): Boolean {
        return NativeBridge.senderStart(
            handle, destIp, destPort, width, height, fps, quality, fecGroup, adaptiveQuality,
            codec.id, bitrateKbps
        ) == 0
    }

    fun stop() {
        if (handle != 0L) NativeBridge.senderStop(handle)
    }

    /** Zero-RGB-conversion push straight from Android Camera2 / CameraX YUV_420_888 Image. */
    fun pushYuvImage(image: Image): Boolean {
        if (handle == 0L || image.planes.size < 3) return false
        val p0 = image.planes[0]
        val p1 = image.planes[1]
        val p2 = image.planes[2]
        return NativeBridge.senderPushYuv420(
            handle, p0.buffer, p1.buffer, p2.buffer,
            image.width, image.height, p0.rowStride, p1.rowStride, p2.rowStride, p1.pixelStride
        ) == 0
    }

    /** Zero-copy push of packed RGBA_8888 direct ByteBuffer (e.g. MediaProjection ImageReader). */
    fun pushRgba(buffer: ByteBuffer, width: Int, height: Int, stride: Int = width * 4): Boolean {
        if (handle == 0L) return false
        return NativeBridge.senderPushPacked(handle, buffer, width, height, stride, 1) == 0
    }

    fun pushAudioPcm16(pcm: ShortArray, numFrames: Int, sampleRate: Int = 48000, channels: Int = 2): Boolean {
        if (handle == 0L) return false
        return NativeBridge.senderPushAudioPcm16(handle, pcm, numFrames, sampleRate, channels) == 0
    }

    // ---- 64-channel AoIP bus ----
    /** Call before start() to choose 48/96 kHz; [latencyMs] 2..2000 may also be changed live. */
    fun configureAudioBus(latencyMs: Int = 20, sampleRate: Int = 48000): Boolean =
        handle != 0L && NativeBridge.senderConfigureAudioBus(handle, sampleRate, latencyMs) == 0

    /** Independent push of one channel (0..63); [mediaTs] = -1 continues the channel automatically. */
    fun pushAudioChannel(channel: Int, samples: FloatArray, count: Int = samples.size, mediaTs: Long = -1L): Boolean =
        handle != 0L && NativeBridge.senderPushAudioChannelF32(handle, channel, samples, count, mediaTs) == 0

    /** Interleaved Float32 push of [channels] channels sharing one master-clock timestamp. */
    fun pushAudioMultichannel(interleaved: FloatArray, channels: Int, frames: Int, mediaTs: Long = -1L): Boolean =
        handle != 0L && NativeBridge.senderPushAudioMultichannelF32(handle, interleaved, channels, frames, mediaTs) == 0

    fun setAudioLatencyMs(latencyMs: Int) { if (handle != 0L) NativeBridge.senderSetAudioLatency(handle, latencyMs) }
    fun masterClockSamples(): Long = if (handle != 0L) NativeBridge.senderMasterClockSamples(handle) else 0L

    fun setQuality(quality: Int) { if (handle != 0L) NativeBridge.senderSetQuality(handle, quality) }
    fun setBitrateKbps(bitrateKbps: Int) { if (handle != 0L) NativeBridge.senderSetBitrate(handle, bitrateKbps) }
    fun forceKeyframe() { if (handle != 0L) NativeBridge.senderForceKeyframe(handle) }
    fun setFecGroup(groupSize: Int) { if (handle != 0L) NativeBridge.senderSetFec(handle, groupSize) }
    fun setAdaptiveQuality(enabled: Boolean, qMin: Int = 35, qMax: Int = 85) {
        if (handle != 0L) NativeBridge.senderSetAdaptive(handle, enabled, qMin, qMax)
    }

    fun stats(): LanVidStreamStats {
        if (handle == 0L) return LanVidStreamStats()
        val o = JSONObject(NativeBridge.senderStatsJson(handle))
        return LanVidStreamStats(
            state = o.optInt("state"),
            width = o.optInt("width"),
            height = o.optInt("height"),
            quality = o.optInt("q"),
            fps = o.optDouble("fps"),
            bitrateMbps = o.optDouble("mbps"),
            encodeMs = o.optDouble("enc_ms"),
            packetLossPct = o.optDouble("loss"),
            audioKbps = o.optDouble("audio_kbps")
        )
    }

    override fun close() {
        if (handle != 0L) {
            NativeBridge.senderDestroy(handle)
            handle = 0L
        }
    }
}
