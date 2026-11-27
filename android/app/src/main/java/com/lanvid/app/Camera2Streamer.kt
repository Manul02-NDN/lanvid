package com.lanvid.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.RggbChannelVector
import android.media.ImageReader
import android.os.Build
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
import kotlin.math.roundToInt

/** Multi-level hardware image stabilization (OIS + EIS / Preview Stabilization on Android 13+). */
enum class StabilizationLevel(
    val title: String,
    val shortBadge: String,
    val cropPct: Int,
    val latencyMs: Int,
    val subtitle: String
) {
    OFF("Вимк.", "EIS Вимк", 0, 0, "Без кропу · 0 мс буфера · штатив / стаб"),
    STANDARD("Стандарт", "OIS Станд", 5, 16, "Оптична OIS · мінімальний кроп (~5%)"),
    CINEMATIC("Кіно", "EIS Кіно", 10, 33, "Плавна відеостабілізація EIS (~10% кроп)"),
    ACTIVE("Активна", "EIS Актив", 15, 50, "Зйомка з рук та в русі (~15% кроп)"),
    ULTRA("Макс", "EIS Макс", 20, 66, "Максимальна OIS + EIS екшн-стабілізація")
}

/**
 * Direct Camera2 YUV_420_888 streamer: pushes plane ByteBuffers directly into liblanvid_core.so
 * via JNI GetDirectBufferAddress with ZERO RGB conversion and ARM64 NEON slice encoding.
 *
 * Includes multi-level hardware stabilization (OIS + EIS), continuous optical/digital zoom,
 * tap-to-focus & AE metering, vertical EV exposure compensation, manual focus pull, and AE/AF lock.
 */
class Camera2Streamer(private val context: Context) {
    private val cameraLock = Any()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val sessionGen = AtomicLong(0L)
    private val isClosing = AtomicBoolean(false)

    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var encodeThread: HandlerThread? = null
    private var encodeHandler: Handler? = null

    private var cameraDevice: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var activeChars: CameraCharacteristics? = null
    private var activeReqBuilder: CaptureRequest.Builder? = null

    // Reusable direct buffers (accessed exclusively on CameraEncode thread)
    private var reusableY: ByteBuffer? = null
    private var reusableU: ByteBuffer? = null
    private var reusableV: ByteBuffer? = null

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

    // Stabilization & Touch state preserved across session restarts
    @Volatile var stabilizationLevel: StabilizationLevel = StabilizationLevel.STANDARD
        private set
    @Volatile var activeHardwareStabilization: StabilizationLevel = StabilizationLevel.OFF
        private set
    @Volatile var currentZoomRatio: Float = 1.0f
        private set
    @Volatile var minZoomRatio: Float = 1.0f
        private set
    @Volatile var maxZoomRatio: Float = 8.0f
        private set

    fun setStreamingEnabled(enabled: Boolean) {
        streamFramesEnabled = enabled
    }

    private fun ensureHandlersLocked(): Pair<Handler, Handler> {
        val bh = if (bgHandler != null && bgThread?.isAlive == true) {
            bgHandler!!
        } else {
            val t = HandlerThread("CameraBackground").apply { start() }
            bgThread = t
            Handler(t.looper).also { bgHandler = it }
        }
        val eh = if (encodeHandler != null && encodeThread?.isAlive == true) {
            encodeHandler!!
        } else {
            val t = HandlerThread("CameraEncode").apply { start() }
            encodeThread = t
            Handler(t.looper).also { encodeHandler = it }
        }
        return bh to eh
    }

    private fun closeResourcesLocked() {
        activeReqBuilder = null
        activeChars = null

        val s = session
        session = null
        if (s != null) {
            try { s.stopRepeating() } catch (_: Exception) {}
            try { s.abortCaptures() } catch (_: Exception) {}
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

    private fun pickBestFpsRange(chars: CameraCharacteristics, targetFps: Int): Range<Int>? {
        val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
        val desired = targetFps.coerceIn(15, 60)
        return ranges.firstOrNull { it.lower == desired && it.upper == desired }
            ?: ranges.firstOrNull { it.upper == desired && it.lower >= (desired / 2) }
            ?: ranges.firstOrNull { it.lower <= desired && it.upper >= desired }
            ?: ranges.minByOrNull { abs(it.upper - desired) }
    }

    private fun applyStabilizationToBuilder(
        req: CaptureRequest.Builder,
        chars: CameraCharacteristics,
        level: StabilizationLevel
    ): StabilizationLevel {
        val oisModes = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION) ?: IntArray(0)
        val videoModes = chars.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: IntArray(0)
        val hasOis = oisModes.contains(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
        val hasVideoEis = videoModes.contains(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON)
        val hasPreviewEis = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            videoModes.contains(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION)

        return when (level) {
            StabilizationLevel.OFF -> {
                if (hasOis) req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF)
                if (hasVideoEis || hasPreviewEis) {
                    req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                }
                StabilizationLevel.OFF
            }
            StabilizationLevel.STANDARD -> {
                if (hasOis) {
                    req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    if (hasVideoEis || hasPreviewEis) {
                        req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                    }
                    StabilizationLevel.STANDARD
                } else if (hasVideoEis) {
                    req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON)
                    StabilizationLevel.STANDARD
                } else {
                    StabilizationLevel.OFF
                }
            }
            StabilizationLevel.CINEMATIC -> {
                if (hasVideoEis) {
                    req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON)
                    if (hasOis) req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    StabilizationLevel.CINEMATIC
                } else if (hasOis) {
                    req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    StabilizationLevel.STANDARD
                } else {
                    StabilizationLevel.OFF
                }
            }
            StabilizationLevel.ACTIVE, StabilizationLevel.ULTRA -> {
                if (hasPreviewEis && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    req.set(
                        CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                        CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_PREVIEW_STABILIZATION
                    )
                    if (hasOis) req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    level
                } else if (hasVideoEis) {
                    req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON)
                    if (hasOis) req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    StabilizationLevel.CINEMATIC
                } else if (hasOis) {
                    req.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON)
                    StabilizationLevel.STANDARD
                } else {
                    StabilizationLevel.OFF
                }
            }
        }
    }

    private fun applyZoomToBuilder(req: CaptureRequest.Builder, chars: CameraCharacteristics, ratio: Float) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val range = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
            val lo = range?.lower ?: 1.0f
            val hi = (range?.upper ?: 8.0f).coerceAtMost(10.0f)
            minZoomRatio = lo
            maxZoomRatio = hi
            val clamped = ratio.coerceIn(lo, hi)
            currentZoomRatio = clamped
            req.set(CaptureRequest.CONTROL_ZOOM_RATIO, clamped)
        } else {
            val maxDigital = (chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 4.0f).coerceAtMost(10.0f)
            minZoomRatio = 1.0f
            maxZoomRatio = maxDigital
            val clamped = ratio.coerceIn(1.0f, maxDigital)
            currentZoomRatio = clamped
            val activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return
            val cropW = (activeArray.width() / clamped).roundToInt().coerceAtLeast(2)
            val cropH = (activeArray.height() / clamped).roundToInt().coerceAtLeast(2)
            val dx = (activeArray.width() - cropW) / 2
            val dy = (activeArray.height() - cropH) / 2
            req.set(CaptureRequest.SCALER_CROP_REGION, Rect(dx, dy, dx + cropW, dy + cropH))
        }
    }

    @SuppressLint("MissingPermission")
    fun start(
        width: Int,
        height: Int,
        frontFacing: Boolean,
        targetFps: Int = 60,
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

        val (handler, encHandler) = synchronized(cameraLock) {
            ensureHandlersLocked()
        }

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

            val actualFrontFacing =
                chars.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT

            val sensorOrientation =
                chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: if (actualFrontFacing) 270 else 90
            val displayDeg = when (displayRotation) {
                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0
            }
            val relRot = if (actualFrontFacing) {
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

            val fpsRange = pickBestFpsRange(chars, targetFps)
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
                        val yBuf = ensureDirectReusable(yPlane.buffer, 0)
                        val uBuf = ensureDirectReusable(uPlane.buffer, 1)
                        val vBuf = ensureDirectReusable(vPlane.buffer, 2)
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
                            actualFrontFacing
                        )
                    }
                } finally {
                    try { img.close() } catch (_: Exception) {}
                }
            }, encHandler)

            synchronized(cameraLock) {
                if (gen != sessionGen.get() || isClosing.get()) {
                    try { imgReader.close() } catch (_: Exception) {}
                    return@post
                }
                reader = imgReader
                activeChars = chars
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
                        if (targets.isEmpty()) {
                            synchronized(cameraLock) {
                                if (cameraDevice === cam) cameraDevice = null
                            }
                            try { cam.close() } catch (_: Exception) {}
                            return
                        }

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
                                activeHardwareStabilization = applyStabilizationToBuilder(this, chars, stabilizationLevel)
                                applyZoomToBuilder(this, chars, currentZoomRatio)
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
                                            activeReqBuilder = reqBuilder
                                        }
                                        try {
                                            s.setRepeatingRequest(reqBuilder.build(), null, handler)
                                        } catch (_: IllegalStateException) {
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
            } catch (e: Exception) {
                reportError("Немає доступу до камери: ${e.message}")
                stop()
            }
        }
    }

    // MARK: - Live Multi-Level Stabilization & Touch Camera Controls

    fun setStabilization(level: StabilizationLevel, onApplied: ((StabilizationLevel) -> Unit)? = null) {
        stabilizationLevel = level
        val h = synchronized(cameraLock) { bgHandler } ?: run {
            onApplied?.invoke(level)
            return
        }
        h.post {
            val applied = synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post
                val resolved = applyStabilizationToBuilder(req, chars, level)
                activeHardwareStabilization = resolved
                try {
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
                resolved
            }
            if (onApplied != null) mainHandler.post { onApplied(applied) }
        }
    }

    fun setZoomRatio(ratio: Float) {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post
                applyZoomToBuilder(req, chars, ratio)
                try {
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
            }
        }
    }

    fun focusAndMeterAt(normX: Float, normY: Float, lockAfter: Boolean = false) {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post
                val activeRect = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return@post

                val cx = (normX.coerceIn(0.05f, 0.95f) * activeRect.width()).roundToInt()
                val cy = (normY.coerceIn(0.05f, 0.95f) * activeRect.height()).roundToInt()
                val halfW = (activeRect.width() * 0.08f).roundToInt().coerceAtLeast(40)
                val halfH = (activeRect.height() * 0.08f).roundToInt().coerceAtLeast(40)
                val region = MeteringRectangle(
                    (cx - halfW).coerceIn(0, activeRect.width() - 2),
                    (cy - halfH).coerceIn(0, activeRect.height() - 2),
                    (halfW * 2).coerceAtMost(activeRect.width()),
                    (halfH * 2).coerceAtMost(activeRect.height()),
                    MeteringRectangle.METERING_WEIGHT_MAX - 1
                )

                val maxAf = chars.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
                val maxAe = chars.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0
                if (maxAf > 0) req.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
                if (maxAe > 0) req.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(region))

                req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                req.set(CaptureRequest.CONTROL_AE_LOCK, lockAfter)
                req.set(
                    CaptureRequest.CONTROL_AF_MODE,
                    if (lockAfter) CaptureRequest.CONTROL_AF_MODE_AUTO else CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                )
                req.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                try {
                    s.capture(req.build(), null, h)
                    req.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
            }
        }
    }

    fun setExposureCompensationEv(ev: Float) {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post
                val range = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: return@post
                val step = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0.333f
                if (step <= 0f) return@post
                val index = (ev / step).roundToInt().coerceIn(range.lower, range.upper)
                req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                req.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, index)
                try {
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
            }
        }
    }

    fun setManualFocusNormalized(focusNormalized: Float) {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post
                val minFocusDist = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                if (minFocusDist > 0f) {
                    req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                    val diopters = (1f - focusNormalized.coerceIn(0f, 1f)) * minFocusDist
                    req.set(CaptureRequest.LENS_FOCUS_DISTANCE, diopters)
                    try {
                        s.setRepeatingRequest(req.build(), null, h)
                    } catch (_: Exception) {}
                }
            }
        }
    }

    fun resetAutoFocusAndExposure() {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                req.set(CaptureRequest.CONTROL_AE_LOCK, false)
                req.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, 0)
                req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                req.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                try {
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
            }
        }
    }

    /**
     * Applies manual exposure (ISO + shutter angle), white balance (Kelvin), and manual focus
     * live to the active Camera2 repeating request when hardware supports manual controls.
     */
    fun applyManualControls(
        manualExposure: Boolean,
        iso: Int,
        shutterAngleDeg: Int,
        fps: Int,
        manualWb: Boolean,
        wbKelvin: Int,
        manualFocus: Boolean,
        focusNormalized: Float
    ) {
        val h = synchronized(cameraLock) { bgHandler } ?: return
        h.post {
            synchronized(cameraLock) {
                val s = session ?: return@post
                val req = activeReqBuilder ?: return@post
                val chars = activeChars ?: return@post

                val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
                val hasManualSensor = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)

                if (manualExposure && hasManualSensor) {
                    req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                    val isoRange = chars.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                    if (isoRange != null) {
                        req.set(CaptureRequest.SENSOR_SENSITIVITY, iso.coerceIn(isoRange.lower, isoRange.upper))
                    }
                    val expRange = chars.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                    if (expRange != null) {
                        val denom = fps.coerceIn(15, 60).toDouble() * (360.0 / shutterAngleDeg.coerceIn(15, 360).toDouble())
                        val expNs = (1_000_000_000.0 / denom.coerceAtLeast(15.0)).toLong()
                        req.set(CaptureRequest.SENSOR_EXPOSURE_TIME, expNs.coerceIn(expRange.lower, expRange.upper))
                    }
                } else {
                    req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                }

                if (manualWb) {
                    val temp = wbKelvin.coerceIn(2500, 10000).toFloat()
                    val norm = (temp - 5600f) / 4400f
                    val rGain = (1.8f + norm * 0.9f).coerceIn(1.0f, 3.0f)
                    val bGain = (1.8f - norm * 0.9f).coerceIn(1.0f, 3.0f)
                    req.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
                    req.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
                    req.set(CaptureRequest.COLOR_CORRECTION_GAINS, RggbChannelVector(rGain, 1.0f, 1.0f, bGain))
                } else {
                    req.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                    req.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_FAST)
                }

                val minFocusDist = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                if (manualFocus && minFocusDist > 0f) {
                    req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                    val diopters = (1f - focusNormalized.coerceIn(0f, 1f)) * minFocusDist
                    req.set(CaptureRequest.LENS_FOCUS_DISTANCE, diopters)
                } else {
                    req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                }

                try {
                    s.setRepeatingRequest(req.build(), null, h)
                } catch (_: Exception) {}
            }
        }
    }

    private fun ensureDirectReusable(src: ByteBuffer, planeIdx: Int): ByteBuffer {
        if (src.isDirect && src.position() == 0) return src
        val dup = src.duplicate()
        dup.rewind()
        val needed = dup.remaining()
        var cached = when (planeIdx) {
            0 -> reusableY
            1 -> reusableU
            else -> reusableV
        }
        if (cached == null || cached.capacity() < needed) {
            cached = ByteBuffer.allocateDirect(needed)
            when (planeIdx) {
                0 -> reusableY = cached
                1 -> reusableU = cached
                else -> reusableV = cached
            }
        }
        cached.clear()
        cached.put(dup)
        cached.flip()
        return cached
    }

    /** Non-blocking stop: never stalls Android Main UI thread (teardown executes on CameraBackground). */
    fun stop(onStopped: (() -> Unit)? = null) {
        isClosing.set(true)
        sessionGen.incrementAndGet()
        val h = synchronized(cameraLock) { bgHandler }
        if (h != null && bgThread?.isAlive == true) {
            h.post {
                synchronized(cameraLock) {
                    closeResourcesLocked()
                }
                onStopped?.invoke()
            }
        } else {
            synchronized(cameraLock) {
                closeResourcesLocked()
            }
            onStopped?.invoke()
        }
    }

    fun release() {
        isClosing.set(true)
        sessionGen.incrementAndGet()
        val tBg: HandlerThread?
        val tEnc: HandlerThread?
        synchronized(cameraLock) {
            closeResourcesLocked()
            tBg = bgThread
            bgThread = null
            bgHandler = null
            tEnc = encodeThread
            encodeThread = null
            encodeHandler = null
        }
        tBg?.quitSafely()
        tEnc?.quitSafely()
    }
}
