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

    fun setAudioTargetMs(targetMs: Double) = lock.read {
        if (handle != 0L) NativeBridge.receiverSetAudioTargetMs(handle, targetMs)
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
