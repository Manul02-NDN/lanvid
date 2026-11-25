package com.lanvid.monitor

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** Quality of the stereo monitor stream from the PC (per channel, like LanVid Studio's bitrate pills). */
enum class StreamQuality(val perChannelKbps: Int, val label: String, val detail: String) {
    K128(128, "128", "128 кбіт/с на канал"),
    K192(192, "192", "192 кбіт/с на канал"),
    K256(256, "256", "256 кбіт/с на канал"),
    LOSSLESS(0, "Lossless", "24-bit PCM ≈ 2.3 Мбіт/с");

    /** kHello value: total stereo kbps or LV_MONITOR_BITRATE_LOSSLESS. */
    val wireKbps: Int get() = if (this == LOSSLESS) MonitorNative.BITRATE_LOSSLESS else perChannelKbps * 2
}

/** Phone playout buffer (Fixed: exactly the delay the musician hears). */
enum class MonitorBuffer(val ms: Int) { MS5(5), MS10(10), MS15(15), MS20(20) }

enum class HpfMode(val hz: Int, val label: String) {
    OFF(0, "Off"), HZ40(40, "40 Гц"), HZ80(80, "80 Гц"), HZ120(120, "120 Гц")
}

/** Server-side DSP of this phone's personal monitor bus (runs on the PC). */
data class DspSettings(
    val hpf: HpfMode = HpfMode.OFF,
    val eqOn: Boolean = true,
    val eqLowDb: Float = 0f,
    val eqMidDb: Float = 0f,
    val eqMidHz: Float = 1200f,
    val eqHighDb: Float = 0f,
    val compOn: Boolean = false,
    val compThresholdDb: Float = -18f,
    val compRatio: Float = 3f,
    val compMakeupDb: Float = 0f,
    val masterVolumeDb: Float = 0f,
    val masterMute: Boolean = false,
) {
    fun params() = floatArrayOf(
        if (hpf == HpfMode.OFF) 80f else hpf.hz.toFloat(), eqLowDb, eqMidHz, eqMidDb, eqHighDb,
        compThresholdDb, compRatio, 5f, 80f, compMakeupDb, masterVolumeDb,
    )

    fun flags() = (if (hpf != HpfMode.OFF) 1 else 0) or (if (eqOn) 2 else 0) or
        (if (compOn) 4 else 0) or (if (masterMute) 8 else 0)

    companion object {
        val midFrequencies = listOf(300f, 600f, 1200f, 2500f, 5000f)
    }
}

/** A LanVid Studio on the LAN (mDNS "_lanvid._udp") or typed in. */
data class Studio(val name: String, val host: String, val port: Int) {
    val id get() = "$host:$port"
}

/** Active local network interface on the smartphone (Wi-Fi / USB-C Ethernet). */
data class NetInterfaceOption(
    val id: String,
    val systemName: String,
    val friendlyName: String,
    val typeTag: String,
    val ipv4: String,
    val speedMbps: Int,
    val isUp: Boolean,
) {
    val displayLabel: String
        get() {
            val name = friendlyName.ifBlank { systemName }
            val speed = if (speedMbps > 0) " · $speedMbps Мбіт/с" else ""
            return "[$typeTag] $name — $ipv4$speed"
        }
}

/** One channel strip; Compose observes each field. */
class ChannelState(val index: Int, name: String) {
    var name by mutableStateOf(name)
    var peakDbfs by mutableFloatStateOf(-120f)
    var gainDb by mutableFloatStateOf(0f)
    var pan by mutableFloatStateOf(0f)
    var mute by mutableStateOf(false)
    var solo by mutableStateOf(false)
}

data class Telemetry(
    val gainReductionDb: Float = 0f,
    val mixPeakDbfs: Float = -120f,
    val bufferMs: Double = 0.0,
    val audioPackets: Long = 0,
    val receiving: Boolean = false,
    val burstFrames: Int = 0,
    val deviceBufferFrames: Int = 0,
    val deviceRate: Int = 0,
    val exclusive: Boolean = false,
    val xruns: Int = 0,
    val clockLocked: Boolean = false,
    val clockOffsetJitterUs: Long = 0,
    val duplicatesRecovered: Long = 0,
    val activeFlowChannels: Int = 0,
    val effectivePlayoutMs: Double = 0.0,
    val wifiRedundancyEnabled: Boolean = false,
)
