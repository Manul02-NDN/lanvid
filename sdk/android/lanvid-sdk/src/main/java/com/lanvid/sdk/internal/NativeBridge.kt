package com.lanvid.sdk.internal

import android.view.Surface
import java.nio.ByteBuffer

internal object NativeBridge {
    init {
        System.loadLibrary("lanvid_core")
        System.loadLibrary("lanvid_jni")
        init()
    }

    @JvmStatic external fun init(): Int
    @JvmStatic external fun version(): String
    @JvmStatic external fun simdInfo(): String
    @JvmStatic external fun scanLanJson(timeoutMs: Int): String

    @JvmStatic external fun senderCreate(): Long
    @JvmStatic external fun senderDestroy(ptr: Long)
    @JvmStatic external fun senderStart(
        ptr: Long, destIp: String, destPort: Int, width: Int, height: Int,
        fps: Int, quality: Int, fecGroup: Int, adaptive: Boolean
    ): Int
    @JvmStatic external fun senderStop(ptr: Long)
    @JvmStatic external fun senderPushYuv420(
        ptr: Long, yBuf: ByteBuffer, uBuf: ByteBuffer, vBuf: ByteBuffer,
        width: Int, height: Int, yStride: Int, uStride: Int, vStride: Int, uvPixelStride: Int
    ): Int
    @JvmStatic external fun senderPushPacked(
        ptr: Long, buf: ByteBuffer, width: Int, height: Int, stride: Int, fmt: Int
    ): Int
    @JvmStatic external fun senderPushAudioPcm16(
        ptr: Long, pcm: ShortArray, frames: Int, rate: Int, channels: Int
    ): Int
    @JvmStatic external fun senderSetQuality(ptr: Long, quality: Int): Int
    @JvmStatic external fun senderSetFec(ptr: Long, groupSize: Int): Int
    @JvmStatic external fun senderSetAdaptive(ptr: Long, enabled: Boolean, qMin: Int, qMax: Int): Int
    @JvmStatic external fun senderStatsJson(ptr: Long): String

    @JvmStatic external fun receiverCreate(): Long
    @JvmStatic external fun receiverDestroy(ptr: Long)
    @JvmStatic external fun receiverStart(
        ptr: Long, port: Int, audioTargetMs: Double, enableAudio: Boolean, deviceName: String
    ): Int
    @JvmStatic external fun receiverStop(ptr: Long)
    @JvmStatic external fun receiverSetSurface(ptr: Long, surface: Surface?)
    @JvmStatic external fun receiverSetVolume(ptr: Long, vol: Float)
    @JvmStatic external fun receiverStatsJson(ptr: Long): String
}
