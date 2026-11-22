package com.lanvid.sdk

import com.lanvid.sdk.internal.NativeBridge
import org.json.JSONArray

enum class LanVidCodec(val id: Int) {
    DCT(0),
    H264(1),
    H265(2)
}

data class LanVidPeer(
    val ip: String,
    val port: Int,
    val name: String,
    val os: String,
    val simd: String,
    val rttMs: Int
)

data class LanVidStreamStats(
    val state: Int = 0,
    val width: Int = 0,
    val height: Int = 0,
    val quality: Int = 0,
    val fps: Double = 0.0,
    val bitrateMbps: Double = 0.0,
    val latencyP50Ms: Double = 0.0,
    val latencyP99Ms: Double = 0.0,
    val encodeMs: Double = 0.0,
    val packetLossPct: Double = 0.0,
    val fecRecovered: Long = 0,
    val audioKbps: Double = 0.0,
    val audioBufferMs: Double = 0.0
)

object LanVidSdk {
    val version: String get() = NativeBridge.version()
    val simdInfo: String get() = NativeBridge.simdInfo()
    val supportedCodecsJson: String get() = NativeBridge.supportedCodecsJson()
}

object LanVidDiscovery {
    fun scan(timeoutMs: Int = 400): List<LanVidPeer> {
        val arr = JSONArray(NativeBridge.scanLanJson(timeoutMs))
        return List(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            LanVidPeer(
                ip = o.optString("ip"),
                port = o.optInt("port", 5000),
                name = o.optString("name", "Receiver"),
                os = o.optString("os", "?"),
                simd = o.optString("simd", "scalar"),
                rttMs = o.optInt("rtt_ms", 0)
            )
        }
    }
}
