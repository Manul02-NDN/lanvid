package com.lanvid.sdk

import android.content.Context
import android.os.Build
import android.util.AttributeSet
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import com.lanvid.sdk.internal.NativeBridge
import org.json.JSONObject
import java.io.Closeable
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

data class LanVidAudioChannelStats(
    val packetsRx: Long = 0, val packetsLost: Long = 0, val fecRecovered: Long = 0,
    val nackRecovered: Long = 0, val nacksSent: Long = 0, val underruns: Long = 0,
    val latePackets: Long = 0, val jitterUs: Int = 0, val active: Boolean = false,
    val levelDbfs: Float = -120f
)

class LanVidReceiver : Closeable {
    private val lock = ReentrantReadWriteLock()
    private var handle: Long = NativeBridge.receiverCreate()

    fun start(
        port: Int = 5000,
        audioTargetMs: Double = 12.0,
        enableAudioPlayback: Boolean = true,
        deviceName: String = Build.MODEL
    ): Boolean = lock.read {
        if (handle == 0L) return false
        NativeBridge.receiverStart(handle, port, audioTargetMs, enableAudioPlayback, deviceName) == 0
    }

    fun stop() = lock.read {
        if (handle != 0L) NativeBridge.receiverStop(handle)
    }

    fun setSurface(surface: Surface?) = lock.read {
        if (handle != 0L) NativeBridge.receiverSetSurface(handle, surface)
    }

    fun setVolume(volume: Float) = lock.read {
        if (handle != 0L) NativeBridge.receiverSetVolume(handle, volume)
    }

    // ---- 64-channel phase-coherent bus ----
    fun configureAudioBus(latencyMs: Int = 20, channels: Int = 64, blockFrames: Int = 240, sampleRate: Int = 48000): Boolean = lock.read {
        handle != 0L && NativeBridge.receiverConfigureAudioBus(handle, sampleRate, latencyMs, channels, blockFrames) == 0
    }

    /** Live latency change 2..2000 ms without reconnecting. */
    fun setAudioLatencyMs(latencyMs: Int) = lock.read {
        if (handle != 0L) NativeBridge.receiverSetAudioLatency(handle, latencyMs)
    }

    /** Latency mode, live: true = Adaptive Wi-Fi (default), false = Fixed (Ethernet / live monitoring). */
    fun setAdaptiveJitter(enabled: Boolean) = lock.read {
        if (handle != 0L) NativeBridge.receiverSetAdaptiveJitter(handle, enabled)
    }

    /** Measured jitter-buffer playout delay in ms (0.0 before playout starts). */
    val effectiveLatencyMs: Double
        get() = lock.read { if (handle != 0L) NativeBridge.receiverGetEffectiveLatencyMs(handle) else 0.0 }

    /** Renders [frames] sample-aligned interleaved Float32 frames for channels 0 until [channels]. */
    fun popAudioMultichannel(out: FloatArray, channels: Int, frames: Int): Int = lock.read {
        if (handle != 0L) NativeBridge.receiverPopAudioMultichannel(handle, out, channels, frames) else -3
    }

    fun channelStats(channel: Int): LanVidAudioChannelStats = lock.read {
        val v = LongArray(10)
        if (handle == 0L || NativeBridge.receiverChannelStats(handle, channel, v) != 0) return LanVidAudioChannelStats()
        LanVidAudioChannelStats(
            packetsRx = v[0], packetsLost = v[1], fecRecovered = v[2], nackRecovered = v[3],
            nacksSent = v[4], underruns = v[5], latePackets = v[6], jitterUs = v[7].toInt(),
            active = v[8] != 0L, levelDbfs = v[9] / 100.0f
        )
    }

    fun stats(): LanVidStreamStats = lock.read {
        if (handle == 0L) return LanVidStreamStats()
        val o = JSONObject(NativeBridge.receiverStatsJson(handle))
        LanVidStreamStats(
            state = o.optInt("state"),
            width = o.optInt("width"),
            height = o.optInt("height"),
            quality = o.optInt("q"),
            fps = o.optDouble("fps"),
            bitrateMbps = o.optDouble("mbps"),
            latencyP50Ms = o.optDouble("lat_p50"),
            latencyP99Ms = o.optDouble("lat_p99"),
            packetLossPct = o.optDouble("loss"),
            fecRecovered = o.optLong("fec"),
            audioKbps = o.optDouble("audio_kbps"),
            audioBufferMs = o.optDouble("audio_buf")
        )
    }

    override fun close() = lock.write {
        if (handle != 0L) {
            NativeBridge.receiverDestroy(handle)
            handle = 0L
        }
    }
}

/** Drop-in SurfaceView automatically binding/unbinding its ANativeWindow to a LanVidReceiver. */
class LanVidSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private var attachedReceiver: LanVidReceiver? = null

    init {
        holder.addCallback(this)
    }

    fun attach(receiver: LanVidReceiver?) {
        attachedReceiver?.setSurface(null)
        attachedReceiver = receiver
        if (holder.surface?.isValid == true) {
            receiver?.setSurface(holder.surface)
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        attachedReceiver?.setSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        attachedReceiver?.setSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        attachedReceiver?.setSurface(null)
    }
}
