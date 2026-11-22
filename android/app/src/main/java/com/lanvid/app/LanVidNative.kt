package com.lanvid.app

import android.view.Surface
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer

data class LanPeer(
    val ip: String,
    val port: Int,
    val name: String,
    val os: String,
    val simd: String,
    val rttMs: Int
)

data class SenderStats(
    val running: Boolean = false,
    val width: Int = 0,
    val height: Int = 0,
    val fps: Double = 0.0,
    val mbps: Double = 0.0,
    val encMs: Double = 0.0,
    val quality: Int = 80,
    val lossPct: Double = 0.0,
    val audioKbps: Double = 0.0,
    val framesSent: Long = 0
)

data class ReceiverStats(
    val running: Boolean = false,
    val width: Int = 0,
    val height: Int = 0,
    val fps: Double = 0.0,
    val mbps: Double = 0.0,
    val latP50: Double = 0.0,
    val latP99: Double = 0.0,
    val lossPct: Double = 0.0,
    val fecRecovered: Long = 0,
    val audioKbps: Double = 0.0,
    val audioBufMs: Double = 0.0
)

object LanVidNative {
    init {
        System.loadLibrary("lanvid_core")
        System.loadLibrary("lanvid_jni")
    }

    @JvmStatic external fun nativeInit(): Int
    @JvmStatic external fun nativeGetVersion(): String
    @JvmStatic external fun nativeGetSimdInfo(): String
    @JvmStatic external fun nativeEnumerateDevices(): String
    @JvmStatic external fun nativeScanLan(timeoutMs: Int): String

    @JvmStatic external fun nativeStartSender(
        destIp: String,
        destPort: Int,
        width: Int,
        height: Int,
        fps: Int,
        quality: Int,
        fecGroup: Int,
        adaptive: Boolean,
        codec: Int,
        bitrateKbps: Int
    ): Boolean

    @JvmStatic external fun nativeSetAudioLatencyMs(latencyMs: Int)

    @JvmStatic external fun nativeStopSender()

    @JvmStatic external fun nativePushYuv420(
        yBuf: ByteBuffer,
        uBuf: ByteBuffer,
        vBuf: ByteBuffer,
        width: Int,
        height: Int,
        yStride: Int,
        uStride: Int,
        vStride: Int,
        uvPixelStride: Int,
        rotationDeg: Int,
        mirrorH: Boolean
    ): Boolean

    @JvmStatic external fun nativePushRgba(
        rgbaBuf: ByteBuffer,
        width: Int,
        height: Int,
        stride: Int
    ): Boolean

    @JvmStatic external fun nativePushAudioPcm16(
        pcm: ShortArray,
        numFrames: Int,
        sampleRate: Int,
        channels: Int
    ): Boolean

    @JvmStatic external fun nativeGetSenderStatsJson(): String

    @JvmStatic external fun nativeStartReceiver(
        port: Int,
        audioTargetMs: Double,
        deviceName: String
    ): Boolean

    @JvmStatic external fun nativeStopReceiver()
    @JvmStatic external fun nativeSetReceiverSurface(surface: Surface?)
    @JvmStatic external fun nativeSetVolume(vol: Float)
    @JvmStatic external fun nativeGetReceiverStatsJson(): String

    fun scanLanPeers(timeoutMs: Int = 450): List<LanPeer> {
        val raw = nativeScanLan(timeoutMs)
        val arr = JSONArray(raw)
        return List(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            LanPeer(
                ip = o.optString("ip"),
                port = o.optInt("port", 5000),
                name = o.optString("name", "Receiver"),
                os = o.optString("os", "?"),
                simd = o.optString("simd", "scalar"),
                rttMs = o.optInt("rtt_ms", 0)
            )
        }
    }

    fun senderStats(): SenderStats {
        val o = JSONObject(nativeGetSenderStatsJson())
        return SenderStats(
            running = o.optInt("running", 0) != 0,
            width = o.optInt("width", 0),
            height = o.optInt("height", 0),
            fps = o.optDouble("fps", 0.0),
            mbps = o.optDouble("mbps", 0.0),
            encMs = o.optDouble("enc_ms", 0.0),
            quality = o.optInt("q", 80),
            lossPct = o.optDouble("loss", 0.0),
            audioKbps = o.optDouble("audio_kbps", 0.0),
            framesSent = o.optLong("frames", 0L)
        )
    }

    fun receiverStats(): ReceiverStats {
        val o = JSONObject(nativeGetReceiverStatsJson())
        return ReceiverStats(
            running = o.optInt("running", 0) != 0,
            width = o.optInt("width", 0),
            height = o.optInt("height", 0),
            fps = o.optDouble("fps", 0.0),
            mbps = o.optDouble("mbps", 0.0),
            latP50 = o.optDouble("lat_p50", 0.0),
            latP99 = o.optDouble("lat_p99", 0.0),
            lossPct = o.optDouble("loss", 0.0),
            fecRecovered = o.optLong("fec", 0L),
            audioKbps = o.optDouble("audio_kbps", 0.0),
            audioBufMs = o.optDouble("audio_buf", 0.0)
        )
    }
}
