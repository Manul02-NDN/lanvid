package com.crosslink.sdk.internal

internal object CrossLinkNativeBridge {
    init {
        System.loadLibrary("crosslink_core")
        System.loadLibrary("crosslink_jni")
        init()
    }

    @JvmStatic external fun init(): Int
    @JvmStatic external fun version(): String
    @JvmStatic external fun scanNodesJson(timeoutMs: Int, discoveryPort: Int): String

    @JvmStatic external fun senderCreate(): Long
    @JvmStatic external fun senderDestroy(ptr: Long)
    @JvmStatic external fun senderStart(
        ptr: Long, destIp: String, destPort: Int, sampleRate: Int, latencyMs: Int, codec: Int
    ): Int
    @JvmStatic external fun senderStop(ptr: Long)
    @JvmStatic external fun senderPushChannelF32(
        ptr: Long, channel: Int, samples: FloatArray, count: Int, mediaTs: Long
    ): Int
    @JvmStatic external fun senderPushMultichannelF32(
        ptr: Long, interleaved: FloatArray, channels: Int, frames: Int, mediaTs: Long
    ): Int
    @JvmStatic external fun senderPushMidi(
        ptr: Long, status: Int, d1: Int, d2: Int, port: Int, mediaTsUs: Long
    ): Int
    @JvmStatic external fun senderSetLatency(ptr: Long, latencyMs: Int): Int
    @JvmStatic external fun senderMasterClockSamples(ptr: Long): Long

    @JvmStatic external fun receiverCreate(): Long
    @JvmStatic external fun receiverDestroy(ptr: Long)
    @JvmStatic external fun receiverStart(
        ptr: Long, bindPort: Int, sampleRate: Int, latencyMs: Int, channels: Int,
        adaptiveJitter: Boolean, deviceName: String
    ): Int
    @JvmStatic external fun receiverStop(ptr: Long)
    @JvmStatic external fun receiverSetLatency(ptr: Long, latencyMs: Int): Int
    @JvmStatic external fun receiverSetAdaptiveJitter(ptr: Long, enabled: Boolean): Int
    @JvmStatic external fun receiverGetEffectiveLatencyMs(ptr: Long): Double
    @JvmStatic external fun receiverPopMultichannel(
        ptr: Long, out: FloatArray, channels: Int, frames: Int
    ): Int
    @JvmStatic external fun receiverChannelStats(ptr: Long, channel: Int, out: LongArray): Int
}
