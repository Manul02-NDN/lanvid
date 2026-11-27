package com.crosslink.sdk

import android.os.Build
import com.crosslink.sdk.internal.CrossLinkNativeBridge
import org.json.JSONArray
import java.io.Closeable
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

data class CrossLinkAudioNode(
    val ip: String,
    val name: String,
    val hostname: String,
    val audioDriver: String,
    val rxPort: Int,
    val txPort: Int,
    val channels: Int,
    val duplex: Boolean,
    val rttMs: Int,
    val os: String
)

data class CrossLinkAudioChannelStats(
    val packetsRx: Long = 0, val packetsLost: Long = 0, val fecRecovered: Long = 0,
    val nackRecovered: Long = 0, val nacksSent: Long = 0, val underruns: Long = 0,
    val latePackets: Long = 0, val jitterUs: Int = 0, val active: Boolean = false,
    val levelDbfs: Float = -120f
)

object CrossLinkSdk {
    val version: String get() = CrossLinkNativeBridge.version()
}

object CrossLinkDiscovery {
    fun scan(timeoutMs: Int = 400, discoveryPort: Int = 0): List<CrossLinkAudioNode> {
        val arr = JSONArray(CrossLinkNativeBridge.scanNodesJson(timeoutMs, discoveryPort))
        return List(arr.length()) { i ->
            val o = arr.getJSONObject(i)
            CrossLinkAudioNode(
                ip = o.optString("ip"),
                name = o.optString("name", "AudioNode"),
                hostname = o.optString("hostname", ""),
                audioDriver = o.optString("audio_driver", "CrossLink"),
                rxPort = o.optInt("rx_port", 5004),
                txPort = o.optInt("tx_port", 5005),
                channels = o.optInt("channels", 64),
                duplex = o.optBoolean("duplex", false),
                rttMs = o.optInt("rtt_ms", 0),
                os = o.optString("os", "?")
            )
        }
    }
}

class CrossLinkSender : Closeable {
    private val lock = ReentrantReadWriteLock()
    private var handle: Long = CrossLinkNativeBridge.senderCreate()

    fun start(
        destIp: String,
        destPort: Int = 5004,
        sampleRate: Int = 48000,
        latencyMs: Int = 20,
        codec: Int = 0
    ): Boolean = lock.read {
        handle != 0L && CrossLinkNativeBridge.senderStart(handle, destIp, destPort, sampleRate, latencyMs, codec) == 0
    }

    fun stop() = lock.read { if (handle != 0L) CrossLinkNativeBridge.senderStop(handle) }

    fun pushAudioChannel(channel: Int, samples: FloatArray, count: Int = samples.size, mediaTs: Long = -1L): Boolean =
        lock.read { handle != 0L && CrossLinkNativeBridge.senderPushChannelF32(handle, channel, samples, count, mediaTs) == 0 }

    fun pushAudioMultichannel(interleaved: FloatArray, channels: Int, frames: Int, mediaTs: Long = -1L): Boolean =
        lock.read { handle != 0L && CrossLinkNativeBridge.senderPushMultichannelF32(handle, interleaved, channels, frames, mediaTs) == 0 }

    fun pushMidi(status: Int, data1: Int, data2: Int, port: Int = 0, mediaTsUs: Long = 0L): Boolean =
        lock.read { handle != 0L && CrossLinkNativeBridge.senderPushMidi(handle, status, data1, data2, port, mediaTsUs) == 0 }

    fun setLatencyMs(latencyMs: Int) = lock.read {
        if (handle != 0L) CrossLinkNativeBridge.senderSetLatency(handle, latencyMs)
    }

    fun masterClockSamples(): Long = lock.read {
        if (handle != 0L) CrossLinkNativeBridge.senderMasterClockSamples(handle) else 0L
    }

    override fun close() = lock.write {
        if (handle != 0L) {
            CrossLinkNativeBridge.senderDestroy(handle)
            handle = 0L
        }
    }
}

class CrossLinkReceiver : Closeable {
    private val lock = ReentrantReadWriteLock()
    private var handle: Long = CrossLinkNativeBridge.receiverCreate()

    fun start(
        bindPort: Int = 5004,
        sampleRate: Int = 48000,
        latencyMs: Int = 20,
        channels: Int = 64,
        adaptiveJitter: Boolean = true,
        deviceName: String = Build.MODEL
    ): Boolean = lock.read {
        handle != 0L && CrossLinkNativeBridge.receiverStart(
            handle, bindPort, sampleRate, latencyMs, channels, adaptiveJitter, deviceName
        ) == 0
    }

    fun stop() = lock.read { if (handle != 0L) CrossLinkNativeBridge.receiverStop(handle) }

    fun setLatencyMs(latencyMs: Int) = lock.read {
        if (handle != 0L) CrossLinkNativeBridge.receiverSetLatency(handle, latencyMs)
    }

    fun setAdaptiveJitter(enabled: Boolean) = lock.read {
        if (handle != 0L) CrossLinkNativeBridge.receiverSetAdaptiveJitter(handle, enabled)
    }

    val effectiveLatencyMs: Double
        get() = lock.read { if (handle != 0L) CrossLinkNativeBridge.receiverGetEffectiveLatencyMs(handle) else 0.0 }

    fun popAudioMultichannel(out: FloatArray, channels: Int, frames: Int): Int = lock.read {
        if (handle != 0L) CrossLinkNativeBridge.receiverPopMultichannel(handle, out, channels, frames) else -3
    }

    fun channelStats(channel: Int): CrossLinkAudioChannelStats = lock.read {
        val v = LongArray(10)
        if (handle == 0L || CrossLinkNativeBridge.receiverChannelStats(handle, channel, v) != 0)
            return CrossLinkAudioChannelStats()
        CrossLinkAudioChannelStats(
            packetsRx = v[0], packetsLost = v[1], fecRecovered = v[2], nackRecovered = v[3],
            nacksSent = v[4], underruns = v[5], latePackets = v[6], jitterUs = v[7].toInt(),
            active = v[8] != 0L, levelDbfs = v[9] / 100.0f
        )
    }

    override fun close() = lock.write {
        if (handle != 0L) {
            CrossLinkNativeBridge.receiverDestroy(handle)
            handle = 0L
        }
    }
}
