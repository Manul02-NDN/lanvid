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
    private val lock = Any()
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null
    @Volatile private var activeRecorder: AudioRecord? = null

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

        synchronized(lock) {
            activeRecorder = recorder
            running.set(true)
            try {
                recorder.startRecording()
            } catch (_: Exception) {
                running.set(false)
                activeRecorder = null
                recorder.release()
                return false
            }
            worker = Thread({
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
                val pcm = ShortArray(chunkFrames * channels)
                try {
                    while (running.get()) {
                        val read = recorder.read(pcm, 0, pcm.size)
                        if (read < 0) {
                            break // Error / stopped / device disconnected
                        }
                        if (read > 0) {
                            val frames = read / channels
                            if (frames > 0) {
                                LanVidNative.nativePushAudioPcm16(pcm, frames, sampleRate, channels)
                            }
                        }
                    }
                } finally {
                    try { recorder.stop() } catch (_: Exception) {}
                    try { recorder.release() } catch (_: Exception) {}
                    if (activeRecorder === recorder) {
                        activeRecorder = null
                    }
                }
            }, "LanVidAudioIn").apply { start() }
        }
        return true
    }

    fun stop() {
        running.set(false)
        val rec: AudioRecord?
        val t: Thread?
        synchronized(lock) {
            rec = activeRecorder
            t = worker
            worker = null
        }
        // Unblock any blocking recorder.read() immediately
        if (rec != null) {
            try { rec.stop() } catch (_: Exception) {}
        }
        try { t?.join(500) } catch (_: InterruptedException) {}
    }
}
