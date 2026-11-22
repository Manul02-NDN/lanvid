package com.lanvid.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder

/**
 * Foreground Service keeping MediaProjection screen capture + optional internal audio
 * alive even when LanVid is minimized in the background.
 */
class ScreenCaptureService : Service() {
    companion object {
        const val ACTION_START = "com.lanvid.app.ACTION_START_SCREEN"
        const val ACTION_STOP = "com.lanvid.app.ACTION_STOP_SCREEN"
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        const val EXTRA_WIDTH = "width"
        const val EXTRA_HEIGHT = "height"
        const val EXTRA_DPI = "dpi"
        const val EXTRA_AUDIO_MODE = "audio_mode" // 0=off, 1=internal, 2=mic
        private const val CHANNEL_ID = "lanvid_capture_channel"
        private const val NOTIF_ID = 7001

        @Volatile
        var isRunning: Boolean = false
            private set
    }

    private var projection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null
    private var handlerThread: HandlerThread? = null
    private var audioStreamer: AudioInputStreamer? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopCapture()
            stopSelf()
            return START_NOT_STICKY
        }

        val notif = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val fgsType = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION or
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            startForeground(NOTIF_ID, notif, fgsType)
        } else {
            startForeground(NOTIF_ID, notif)
        }

        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, 0) ?: 0
        @Suppress("DEPRECATION")
        val data: Intent? = intent?.getParcelableExtra(EXTRA_RESULT_DATA)
        val width = (intent?.getIntExtra(EXTRA_WIDTH, 1280) ?: 1280) and 0x7FFE
        val height = (intent?.getIntExtra(EXTRA_HEIGHT, 720) ?: 720) and 0x7FFE
        val dpi = intent?.getIntExtra(EXTRA_DPI, 320) ?: 320
        val audioMode = intent?.getIntExtra(EXTRA_AUDIO_MODE, 0) ?: 0

        if (data == null) {
            stopSelf()
            return START_NOT_STICKY
        }

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val mp = mpm.getMediaProjection(resultCode, data) ?: run {
            stopSelf()
            return START_NOT_STICKY
        }
        projection = mp

        val ht = HandlerThread("LanVidScreenCap").apply { start() }
        handlerThread = ht
        val handler = Handler(ht.looper)

        val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3)
        reader.setOnImageAvailableListener({ r ->
            val img = r.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val plane = img.planes[0]
                LanVidNative.nativePushRgba(plane.buffer, img.width, img.height, plane.rowStride)
            } finally {
                img.close()
            }
        }, handler)
        imageReader = reader

        virtualDisplay = mp.createVirtualDisplay(
            "LanVidScreen",
            width,
            height,
            dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            reader.surface,
            null,
            handler
        )

        if (audioMode == 1 || audioMode == 2) {
            audioStreamer = AudioInputStreamer().also {
                it.start(if (audioMode == 1) mp else null)
            }
        }
        isRunning = true
        return START_STICKY
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val ch = NotificationChannel(
            CHANNEL_ID,
            "LanVid Screen Streaming",
            NotificationManager.IMPORTANCE_LOW
        )
        nm.createNotificationChannel(ch)
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("LanVid Studio")
            .setContentText("Трансляція екрана в локальній мережі активна")
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setOngoing(true)
            .build()
    }

    private fun stopCapture() {
        isRunning = false
        audioStreamer?.stop()
        audioStreamer = null
        virtualDisplay?.release()
        virtualDisplay = null
        imageReader?.close()
        imageReader = null
        projection?.stop()
        projection = null
        handlerThread?.quitSafely()
        handlerThread = null
        LanVidNative.nativeStopSender()
    }

    override fun onDestroy() {
        stopCapture()
        super.onDestroy()
    }
}
