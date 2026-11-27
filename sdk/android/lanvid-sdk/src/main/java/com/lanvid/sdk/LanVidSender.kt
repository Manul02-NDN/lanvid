package com.lanvid.sdk

import android.media.Image
import com.lanvid.sdk.internal.NativeBridge
import org.json.JSONObject
import java.io.Closeable
import java.nio.ByteBuffer
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

class LanVidSender : Closeable {
    private val lock = ReentrantReadWriteLock()
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
    ): Boolean = lock.read {
        if (handle == 0L) return false
        NativeBridge.senderStart(
            handle, destIp, destPort, width, height, fps, quality, fecGroup, adaptiveQuality,
            codec.id, bitrateKbps
        ) == 0
    }

    fun stop() = lock.read {
        if (handle != 0L) NativeBridge.senderStop(handle)
    }

    /**
     * Zero-RGB-conversion push straight from Android Camera2 / CameraX YUV_420_888 Image.
     * Supports clockwise rotation (0/90/180/270 deg) and horizontal mirror (for front camera).
     */
    fun pushYuvImage(
        image: Image,
        rotationDegrees: Int = 0,
        mirrorHorizontal: Boolean = false
    ): Boolean = lock.read {
        if (handle == 0L || image.planes.size < 3) return false
        val p0 = image.planes[0]
        val p1 = image.planes[1]
        val p2 = image.planes[2]
        val yBuf = ensureDirect(p0.buffer)
        val uBuf = ensureDirect(p1.buffer)
        val vBuf = ensureDirect(p2.buffer)
        NativeBridge.senderPushYuv420Ex(
            handle, yBuf, uBuf, vBuf,
            image.width, image.height, p0.rowStride, p1.rowStride, p2.rowStride, p1.pixelStride,
            rotationDegrees, mirrorHorizontal
        ) == 0
    }

    private fun ensureDirect(src: ByteBuffer): ByteBuffer {
        if (src.isDirect && src.position() == 0) return src
        val dup = src.duplicate()
        dup.rewind()
        val direct = ByteBuffer.allocateDirect(dup.remaining())
        direct.put(dup)
        direct.rewind()
        return direct
    }

    /** Zero-copy push of packed RGBA_8888 direct ByteBuffer (e.g. MediaProjection ImageReader). */
    fun pushRgba(buffer: ByteBuffer, width: Int, height: Int, stride: Int = width * 4): Boolean = lock.read {
        if (handle == 0L) return false
        val direct = ensureDirect(buffer)
        NativeBridge.senderPushPacked(handle, direct, width, height, stride, 1) == 0
    }

    /** Push 1-2 channel stereo PCM16 audio alongside video. */
    fun pushAudioPcm16(pcm: ShortArray, numFrames: Int, sampleRate: Int = 48000, channels: Int = 2): Boolean = lock.read {
        if (handle == 0L || channels !in 1..2) return false
        NativeBridge.senderPushAudioPcm16(handle, pcm, numFrames, sampleRate, channels) == 0
    }

    fun setQuality(quality: Int) = lock.read { if (handle != 0L) NativeBridge.senderSetQuality(handle, quality) }
    fun setBitrateKbps(bitrateKbps: Int) = lock.read { if (handle != 0L) NativeBridge.senderSetBitrate(handle, bitrateKbps) }
    fun forceKeyframe() = lock.read { if (handle != 0L) NativeBridge.senderForceKeyframe(handle) }
    fun setFecGroup(groupSize: Int) = lock.read { if (handle != 0L) NativeBridge.senderSetFec(handle, groupSize) }
    fun setAdaptiveQuality(enabled: Boolean, qMin: Int = 35, qMax: Int = 85) = lock.read {
        if (handle != 0L) NativeBridge.senderSetAdaptive(handle, enabled, qMin, qMax)
    }

    fun stats(): LanVidStreamStats = lock.read {
        if (handle == 0L) return LanVidStreamStats()
        val o = JSONObject(NativeBridge.senderStatsJson(handle))
        LanVidStreamStats(
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

    override fun close() = lock.write {
        if (handle != 0L) {
            NativeBridge.senderDestroy(handle)
            handle = 0L
        }
    }
}
