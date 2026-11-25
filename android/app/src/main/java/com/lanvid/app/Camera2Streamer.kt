package com.lanvid.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Range
import android.util.Size
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.abs

/**
 * Direct Camera2 YUV_420_888 streamer: pushes plane ByteBuffers directly into liblanvid_core.so
 * via JNI GetDirectBufferAddress with ZERO RGB conversion and ARM64 NEON slice encoding.
 *
 * Thread-safe & race-free lifecycle:
 *  - Serialized on a dedicated "CameraBackground" HandlerThread + cameraLock
 *  - Monotonic sessionGen counter discards stale callbacks from rapid lens switches / lifecycle stops
 *  - Strict aspect-ratio matching (16:9) prevents 4:3/16:9 stretching on preview and network output
 */
class Camera2Streamer(private val context: Context) {
    private val cameraLock = Any()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val sessionGen = AtomicLong(0L)
    private val isClosing = AtomicBoolean(false)

    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null

    @Volatile private var streamFramesEnabled: Boolean = false
    @Volatile var outputWidth: Int = 1280
        private set
    @Volatile var outputHeight: Int = 720
        private set
    @Volatile var sensorBufferWidth: Int = 1280
        private set
    @Volatile var sensorBufferHeight: Int = 720
        private set
    @Volatile var streamRotationDeg: Int = 90
        private set

    fun setStreamingEnabled(enabled: Boolean) {
        streamFramesEnabled = enabled
    }

    private fun ensureBackgroundHandlerLocked(): Handler {
        val existing = bgHandler
        val thread = bgThread
        if (existing != null && thread != null && thread.isAlive) {
            return existing
        }
        val t = HandlerThread("CameraBackground").apply { start() }
        val h = Handler(t.looper)
        bgThread = t
        bgHandler = h
        return h
    }

    private fun closeResourcesLocked() {
        val s = session
        session = null
        if (s != null) {
            try { s.stopRepeating() } catch (_: CameraAccessException) {} catch (_: IllegalStateException) {} catch (_: Exception) {}
            try { s.abortCaptures() } catch (_: CameraAccessException) {} catch (_: IllegalStateException) {} catch (_: Exception) {}
            try { s.close() } catch (_: Exception) {}
        }

        val cam = cameraDevice
        cameraDevice = null
        if (cam != null) {
            try { cam.close() } catch (_: Exception) {}
        }

        val r = reader
        reader = null
        if (r != null) {
            try { r.close() } catch (_: Exception) {}
        }
    }

    private fun pickStrictAspectSize(
        yuvSizes: Array<Size>?,
        previewSizes: Array<Size>?,
        reqW: Int,
        reqH: Int
    ): Size {
        if (yuvSizes.isNullOrEmpty()) return Size(reqW, reqH)
        val targetRatio = reqW.toDouble() / reqH.coerceAtLeast(1).toDouble()
        val previewSet = previewSizes?.toSet()

        // Prefer sizes supported by BOTH YUV_420_888 and SurfaceTexture that strictly match target aspect ratio (e.g. 16:9).
        val strictCommon = yuvSizes.filter { s ->
            val r = s.width.toDouble() / s.height.coerceAtLeast(1).toDouble()
            abs(r - targetRatio) <= 0.03 && (previewSet == null || previewSet.contains(s))
        }
        val strictAny = if (strictCommon.isNotEmpty()) strictCommon else yuvSizes.filter { s ->
            val r = s.width.toDouble() / s.height.coerceAtLeast(1).toDouble()
            abs(r - targetRatio) <= 0.03
        }
        val pool = if (strictAny.isNotEmpty()) strictAny else yuvSizes.toList()
        return pool.minByOrNull { s ->
            abs(s.width - reqW) + abs(s.height - reqH)
        } ?: Size(reqW, reqH)
    }

    private fun pickBestFpsRange(chars: CameraCharacteristics): Range<Int>? {
        val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
        return ranges.firstOrNull { it.lower == 30 && it.upper == 60 }
            ?: ranges.firstOrNull { it.lower >= 30 && it.upper >= 30 }
            ?: ranges.maxByOrNull { it.upper }
    }

    @SuppressLint("MissingPermission")
    fun start(
        width: Int,
        height: Int,
        frontFacing: Boolean,
        displayRotation: Int = Surface.ROTATION_0,
        previewSurface: Surface? = null,
        streamEnabled: Boolean = true,
        onPreviewConfigured: ((bufW: Int, bufH: Int, orientedW: Int, orientedH: Int, rotDeg: Int) -> Unit)? = null,
        onError: (String) -> Unit
    ) {
        val gen = sessionGen.incrementAndGet()
        isClosing.set(false)
        streamFramesEnabled = streamEnabled

        val reportError: (String) -> Unit = { msg ->
            mainHandler.post {
                if (gen == sessionGen.get() && !isClosing.get()) onError(msg)
            }
        }

        val handler: Handler = synchronized(cameraLock) {
            ensureBackgroundHandlerLocked()
        }

        // Perform all CameraManager queries, teardown, and openCamera on CameraBackground (ZERO Main-thread blocking!)
        handler.post {
            if (gen != sessionGen.get() || isClosing.get()) return@post
            synchronized(cameraLock) {
                closeResourcesLocked()
            }

            val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val targetFacing = if (frontFacing) {
                CameraCharacteristics.LENS_FACING_FRONT
            } else {
                CameraCharacteristics.LENS_FACING_BACK
            }
            val camId = try {
                mgr.cameraIdList.firstOrNull { id ->
                    mgr.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == targetFacing
                } ?: mgr.cameraIdList.firstOrNull()
            } catch (e: Exception) {
                reportError("Немає доступу до списку камер: ${e.message}")
                return@post
            }

            if (camId == null) {
                reportError("Камеру не знайдено")
                return@post
            }

            val chars = try {
                mgr.getCameraCharacteristics(camId)
            } catch (e: Exception) {
                reportError("Не вдалося прочитати параметри камери: ${e.message}")
                return@post
            }

            val sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: if (frontFacing) 270 else 90
            val displayDeg = when (displayRotation) {
                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0
            }
            val relRot = if (frontFacing) {
                (sensorOrientation + displayDeg) % 360
            } else {
                (sensorOrientation - displayDeg + 360) % 360
            }

            val streamMap = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val yuvSizes = streamMap?.getOutputSizes(ImageFormat.YUV_420_888)
            val previewSizes = streamMap?.getOutputSizes(SurfaceTexture::class.java)
            val chosenSize = pickStrictAspectSize(yuvSizes, previewSizes, width, height)

            val swapped = (relRot == 90 || relRot == 270)
            val orientedW = if (swapped) chosenSize.height else chosenSize.width
            val orientedH = if (swapped) chosenSize.width else chosenSize.height

            sensorBufferWidth = chosenSize.width
            sensorBufferHeight = chosenSize.height
            outputWidth = orientedW
            outputHeight = orientedH
            streamRotationDeg = relRot

            mainHandler.post {
                if (gen == sessionGen.get() && !isClosing.get()) {
                    onPreviewConfigured?.invoke(chosenSize.width, chosenSize.height, orientedW, orientedH, relRot)
                }
            }

            val fpsRange = pickBestFpsRange(chars)
            if (gen != sessionGen.get() || isClosing.get()) return@post

            val imgReader = try {
                ImageReader.newInstance(chosenSize.width, chosenSize.height, ImageFormat.YUV_420_888, 3)
            } catch (e: Exception) {
                reportError("Помилка ImageReader: ${e.message}")
                return@post
            }

            imgReader.setOnImageAvailableListener({ r ->
                val img = try { r.acquireLatestImage() } catch (_: Exception) { null } ?: return@setOnImageAvailableListener
                try {
                    if (gen != sessionGen.get() || isClosing.get()) return@setOnImageAvailableListener
                    if (streamFramesEnabled && img.planes.size >= 3) {
                        val yPlane = img.planes[0]
                        val uPlane = img.planes[1]
                        val vPlane = img.planes[2]
                        val yBuf = ensureDirect(yPlane.buffer)
                        val uBuf = ensureDirect(uPlane.buffer)
                        val vBuf = ensureDirect(vPlane.buffer)
                        LanVidNative.nativePushYuv420(
                            yBuf,
                            uBuf,
                            vBuf,
                            img.width,
                            img.height,
                            yPlane.rowStride,
                            uPlane.rowStride,
                            vPlane.rowStride,
                            uPlane.pixelStride,
                            relRot,
                            frontFacing
                        )
                    }
                } finally {
                    try { img.close() } catch (_: Exception) {}
                }
            }, handler)

            synchronized(cameraLock) {
                if (gen != sessionGen.get() || isClosing.get()) {
                    try { imgReader.close() } catch (_: Exception) {}
                    return@post
                }
                reader = imgReader
            }

            try {
                mgr.openCamera(camId, object : CameraDevice.StateCallback() {
                    override fun onOpened(cam: CameraDevice) {
                        if (gen != sessionGen.get() || isClosing.get()) {
                            try { cam.close() } catch (_: Exception) {}
                            return
                        }
                        val targets = synchronized(cameraLock) {
                            if (gen != sessionGen.get() || isClosing.get()) {
                                try { cam.close() } catch (_: Exception) {}
                                return
                            }
                            cameraDevice = cam
                            buildList {
                                if (imgReader.surface.isValid) add(imgReader.surface)
                                if (previewSurface != null && previewSurface.isValid) add(previewSurface)
                            }
                        }
                        if (targets.isEmpty()) return

                        val reqBuilder = try {
                            cam.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                                targets.forEach { addTarget(it) }
                                if (fpsRange != null) {
                                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
                                }
                                set(
                                    CaptureRequest.CONTROL_AF_MODE,
                                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                                )
                            }
                        } catch (_: IllegalStateException) {
                            return
                        } catch (e: CameraAccessException) {
                            reportError("Помилка запиту камери: ${e.message}")
                            return
                        }

                        try {
                            @Suppress("DEPRECATION")
                            cam.createCaptureSession(
                                targets,
                                object : CameraCaptureSession.StateCallback() {
                                    override fun onConfigured(s: CameraCaptureSession) {
                                        if (gen != sessionGen.get() || isClosing.get()) {
                                            try { s.close() } catch (_: Exception) {}
                                            return
                                        }
                                        synchronized(cameraLock) {
                                            if (gen != sessionGen.get() || isClosing.get()) {
                                                try { s.close() } catch (_: Exception) {}
                                                return
                                            }
                                            session = s
                                        }
                                        try {
                                            s.setRepeatingRequest(reqBuilder.build(), null, handler)
                                        } catch (_: IllegalStateException) {
                                            // Session/device closed during rapid switch or pause — ignore silently.
                                        } catch (e: CameraAccessException) {
                                            reportError("Помилка запуску камери: ${e.message}")
                                        } catch (e: Exception) {
                                            reportError("Помилка запуску камери: ${e.message}")
                                        }
                                    }

                                    override fun onConfigureFailed(s: CameraCaptureSession) {
                                        reportError("Не вдалося налаштувати Camera2 сесію")
                                    }
                                },
                                handler
                            )
                        } catch (_: IllegalStateException) {
                            // Stale CameraDevice closed while creating session; ignore cleanly.
                        } catch (e: CameraAccessException) {
                            reportError("Не вдалося створити сесію камери: ${e.message}")
                        }
                    }

                    override fun onDisconnected(cam: CameraDevice) {
                        synchronized(cameraLock) {
                            if (cameraDevice === cam) cameraDevice = null
                        }
                        try { cam.close() } catch (_: Exception) {}
                    }

                    override fun onError(cam: CameraDevice, error: Int) {
                        synchronized(cameraLock) {
                            if (cameraDevice === cam) cameraDevice = null
                        }
                        try { cam.close() } catch (_: Exception) {}
                        reportError("Camera2 error=$error")
                    }
                }, handler)
            } catch (_: IllegalStateException) {
                // Ignored if superseded by newer start()/stop().
            } catch (e: Exception) {
                reportError("Немає доступу до камери: ${e.message}")
                stop()
            }
        }
    }

    private fun ensureDirect(src: ByteBuffer): ByteBuffer {
        if (src.isDirect && src.position() == 0) return src
        val dup = src.duplicate()
        dup.rewind()
        val direct = ByteBuffer.allocateDirect(dup.remaining())
        direct.put(dup)
        direct.rewind()
        return direct
    }

    /** Non-blocking stop: never stalls Android Main UI thread (teardown executes on CameraBackground). */
    fun stop() {
        isClosing.set(true)
        sessionGen.incrementAndGet()
        val h = synchronized(cameraLock) { bgHandler }
        if (h != null && bgThread?.isAlive == true) {
            h.post {
                synchronized(cameraLock) {
                    closeResourcesLocked()
                }
            }
        } else {
            synchronized(cameraLock) {
                closeResourcesLocked()
            }
        }
    }

    fun release() {
        isClosing.set(true)
        sessionGen.incrementAndGet()
        val t: HandlerThread?
        synchronized(cameraLock) {
            closeResourcesLocked()
            t = bgThread
            bgThread = null
            bgHandler = null
        }
        t?.quitSafely()
    }
}
