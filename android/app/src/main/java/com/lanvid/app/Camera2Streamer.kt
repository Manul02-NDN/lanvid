package com.lanvid.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.util.Size
import android.view.Surface
import java.nio.ByteBuffer
import kotlin.math.abs

/**
 * Direct Camera2 YUV_420_888 streamer: pushes plane ByteBuffers directly into liblanvid_core.so
 * via JNI GetDirectBufferAddress with ZERO RGB conversion and ARM64 NEON slice encoding.
 */
class Camera2Streamer(private val context: Context) {
    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    @Volatile private var streamFramesEnabled: Boolean = false

    fun setStreamingEnabled(enabled: Boolean) {
        streamFramesEnabled = enabled
    }

    @SuppressLint("MissingPermission")
    fun start(
        width: Int,
        height: Int,
        frontFacing: Boolean,
        previewSurface: Surface? = null,
        streamEnabled: Boolean = true,
        onError: (String) -> Unit
    ) {
        stop()
        streamFramesEnabled = streamEnabled
        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val targetFacing = if (frontFacing) {
            CameraCharacteristics.LENS_FACING_FRONT
        } else {
            CameraCharacteristics.LENS_FACING_BACK
        }
        val camId = mgr.cameraIdList.firstOrNull { id ->
            mgr.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == targetFacing
        } ?: mgr.cameraIdList.firstOrNull()

        if (camId == null) {
            onError("Камеру не знайдено")
            return
        }

        val chars = mgr.getCameraCharacteristics(camId)
        val sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: if (frontFacing) 270 else 90
        val streamMap = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val supportedSizes = streamMap?.getOutputSizes(ImageFormat.YUV_420_888)
        val chosenSize: Size = if (!supportedSizes.isNullOrEmpty()) {
            supportedSizes.minByOrNull { s ->
                abs(s.width - width) + abs(s.height - height)
            } ?: Size(width, height)
        } else {
            Size(width, height)
        }

        val thread = HandlerThread("LanVidCamera2").apply { start() }
        val handler = Handler(thread.looper)
        bgThread = thread
        bgHandler = handler

        val imgReader = ImageReader.newInstance(chosenSize.width, chosenSize.height, ImageFormat.YUV_420_888, 3)
        imgReader.setOnImageAvailableListener({ r ->
            val img = r.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
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
                        sensorOrientation,
                        frontFacing
                    )
                }
            } finally {
                img.close()
            }
        }, handler)
        reader = imgReader

        try {
            mgr.openCamera(camId, object : CameraDevice.StateCallback() {
                override fun onOpened(cam: CameraDevice) {
                    cameraDevice = cam
                    val targets = buildList {
                        add(imgReader.surface)
                        if (previewSurface != null && previewSurface.isValid) add(previewSurface)
                    }
                    val reqBuilder = cam.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                        targets.forEach { addTarget(it) }
                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(30, 60))
                        set(
                            CaptureRequest.CONTROL_AF_MODE,
                            CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
                        )
                    }
                    @Suppress("DEPRECATION")
                    cam.createCaptureSession(
                        targets,
                        object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(s: CameraCaptureSession) {
                                session = s
                                try {
                                    s.setRepeatingRequest(reqBuilder.build(), null, handler)
                                } catch (e: Exception) {
                                    onError("Помилка запуску камери: ${e.message}")
                                }
                            }

                            override fun onConfigureFailed(s: CameraCaptureSession) {
                                onError("Не вдалося налаштувати Camera2 сесію")
                            }
                        },
                        handler
                    )
                }

                override fun onDisconnected(cam: CameraDevice) {
                    cam.close()
                }

                override fun onError(cam: CameraDevice, error: Int) {
                    cam.close()
                    onError("Camera2 error=$error")
                }
            }, handler)
        } catch (e: Exception) {
            onError("Немає доступу до камери: ${e.message}")
            stop()
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

    fun stop() {
        try { session?.close() } catch (_: Exception) {}
        session = null
        try { cameraDevice?.close() } catch (_: Exception) {}
        cameraDevice = null
        try { reader?.close() } catch (_: Exception) {}
        reader = null
        bgThread?.quitSafely()
        bgThread = null
        bgHandler = null
    }
}
