package com.lanvid.monitor

/** JNI bridge to the native engine (LanVid core lv_monitor_client_* + AAudio low-latency output). */
object MonitorNative {
    init {
        System.loadLibrary("lanvid_monitor")
    }

    @JvmStatic external fun nativeCreate(): Long
    @JvmStatic external fun nativeDestroy(handle: Long)

    /** 0 = connected (AAudio running), < 0 = LanVid error code (-2 = connected but audio output failed). */
    @JvmStatic external fun nativeConnect(handle: Long, host: String, port: Int, name: String, bitrateKbps: Int, latencyMs: Int): Int
    @JvmStatic external fun nativeDisconnect(handle: Long)
    @JvmStatic external fun nativeSetChannelMix(handle: Long, channel: Int, gainDb: Float, pan: Float, mute: Boolean, solo: Boolean)

    /** params = [hpfHz, eqLow, eqMidHz, eqMid, eqHigh, compThresh, compRatio, compAttack, compRelease, compMakeup, masterVol];
     *  flags: bit0 HPF, bit1 EQ, bit2 compressor, bit3 master mute. */
    @JvmStatic external fun nativeSetDsp(handle: Long, params: FloatArray, flags: Int)
    @JvmStatic external fun nativeSetStream(handle: Long, bitrateKbps: Int, latencyMs: Int): Int
    @JvmStatic external fun nativeSetWifiRedundancy(handle: Long, enabled: Boolean): Int
    @JvmStatic external fun nativeSetSubscribedMask(handle: Long, channelMask: Long): Int

    /** Fills names (64 x 16 UTF-8 bytes), peaks (dBFS) and telemetry (10 values); returns the channel count. */
    @JvmStatic external fun nativeGetMeta(handle: Long, names: ByteArray, peaks: ByteArray, telemetry: DoubleArray): Int

    /** Fills out[8] = { clockLocked, clockOffsetJitterUs, duplicatesRecovered, activeFlowChannels,
     *                   effectivePlayoutMs, wifiRedundancyEnabled, flowPacketsRx, nacksSent }. */
    @JvmStatic external fun nativeGetDanteTelemetry(handle: Long, out: DoubleArray): Int

    fun setWifiRedundancy(handle: Long, enabled: Boolean): Int = nativeSetWifiRedundancy(handle, enabled)
    fun setSubscribedMask(handle: Long, channelMask: Long): Int = nativeSetSubscribedMask(handle, channelMask)
    fun getDanteTelemetry(handle: Long, out: DoubleArray): Int = nativeGetDanteTelemetry(handle, out)

    const val BITRATE_LOSSLESS = 0xFFFF
}
