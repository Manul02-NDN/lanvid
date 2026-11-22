package com.lanvid.app

import android.annotation.SuppressLint
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.projection.MediaProjection
import android.os.Build
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Low-latency 48 kHz Stereo PCM16 audio streamer (5 ms = 240 frames per UDP packet)
 * supporting both Microphone and Android 10+ internal system audio (AudioPlaybackCapture).
 */
class AudioInputStreamer {
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null

    @SuppressLint("MissingPermission")
    fun start(mediaProjection: MediaProjection? = null): Boolean {
        stop()
        val sampleRate = 48000
        val channels = 2
        val chunkFrames = 240 // 5.0 ms per UDP packet
        val minBuf = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_STEREO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(chunkFrames * channels * 4)

        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()

        val recorder: AudioRecord = try {
            if (mediaProjection != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val capCfg = AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
                    .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(AudioAttributes.USAGE_GAME)
                    .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                    .build()
                AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(minBuf)
                    .setAudioPlaybackCaptureConfig(capCfg)
                    .build()
            } else {
                AudioRecord.Builder()
                    .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(minBuf)
                    .build()
            }
        } catch (_: Exception) {
            return false
        }

        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            recorder.release()
            return false
        }

        running.set(true)
        recorder.startRecording()
        worker = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            val pcm = ShortArray(chunkFrames * channels)
            while (running.get()) {
                val read = recorder.read(pcm, 0, pcm.size)
                if (read > 0) {
                    val frames = read / channels
                    if (frames > 0) {
                        LanVidNative.nativePushAudioPcm16(pcm, frames, sampleRate, channels)
                    }
                }
            }
            try { recorder.stop() } catch (_: Exception) {}
            recorder.release()
        }, "LanVidAudioIn").apply { start() }
        return true
    }

    fun stop() {
        running.set(false)
        worker?.join(400)
        worker = null
    }
}
