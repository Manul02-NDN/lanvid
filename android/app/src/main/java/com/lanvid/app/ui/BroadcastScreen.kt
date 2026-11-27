package com.lanvid.app.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.media.projection.MediaProjectionManager
import android.os.BatteryManager
import android.os.Build
import android.util.DisplayMetrics
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.TextureView
import android.view.WindowManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.lanvid.app.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.max
import kotlin.math.min
import kotlin.random.Random

private enum class TallyVisual(val badgeLabel: String) {
    OFF("STBY"),
    PREVIEW("PRV"),
    PROGRAM("PGM")
}

private data class TrafficState(
    val live: Boolean = false,
    val elapsedSec: Long = 0L,
    val instantMbps: Double = 0.0,
    val peakMbps: Double = 0.0,
    val totalBytes: Double = 0.0,
    val historyMbps: List<Float> = emptyList()
) {
    val timerText: String
        get() {
            val h = elapsedSec / 3600
            val m = (elapsedSec % 3600) / 60
            val s = elapsedSec % 60
            return "%02d:%02d:%02d".format(h, m, s)
        }
    val instantMBps: Double get() = instantMbps / 8.0
    val totalDataText: String
        get() = when {
            totalBytes >= 1_000_000_000.0 -> "%.2f GB".format(totalBytes / 1_000_000_000.0)
            totalBytes >= 1_000_000.0 -> "%.1f MB".format(totalBytes / 1_000_000.0)
            else -> "%.0f KB".format(totalBytes / 1_000.0)
        }
}

private fun getDisplayRotation(context: Context): Int {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        runCatching { context.display.rotation }.getOrDefault(Surface.ROTATION_0)
    } else {
        @Suppress("DEPRECATION")
        (context.getSystemService(Context.WINDOW_SERVICE) as? WindowManager)?.defaultDisplay?.rotation
            ?: Surface.ROTATION_0
    }
}

/**
 * Applies a mathematically distortion-free aspect-ratio Matrix to TextureView (Crop or Fit),
 * eliminating vertical/horizontal camera stretching on tall 20:9 / 19.5:9 screens.
 */
private fun configureCameraTextureTransform(
    textureView: TextureView,
    viewW: Int,
    viewH: Int,
    bufW: Int,
    bufH: Int,
    orientedW: Int,
    orientedH: Int,
    displayRotation: Int,
    cropToFill: Boolean
) {
    if (viewW <= 0 || viewH <= 0 || bufW <= 0 || bufH <= 0 || orientedW <= 0 || orientedH <= 0) return
    val vw = viewW.toFloat()
    val vh = viewH.toFloat()
    val cx = vw / 2f
    val cy = vh / 2f
    val matrix = Matrix()

    if (displayRotation == Surface.ROTATION_90 || displayRotation == Surface.ROTATION_270) {
        val viewRect = RectF(0f, 0f, vw, vh)
        val bufferRect = RectF(0f, 0f, bufH.toFloat(), bufW.toFloat())
        bufferRect.offset(cx - bufferRect.centerX(), cy - bufferRect.centerY())
        matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
        val scale = if (cropToFill) {
            max(vh / bufH.toFloat(), vw / bufW.toFloat())
        } else {
            min(vh / bufH.toFloat(), vw / bufW.toFloat())
        }
        matrix.postScale(scale, scale, cx, cy)
        matrix.postRotate(90f * (displayRotation - 2), cx, cy)
    } else {
        val ow = orientedW.toFloat()
        val oh = orientedH.toFloat()
        val targetScale = if (cropToFill) {
            max(vw / ow, vh / oh)
        } else {
            min(vw / ow, vh / oh)
        }
        val sx = (ow * targetScale) / vw
        val sy = (oh * targetScale) / vh
        matrix.setScale(sx, sy, cx, cy)
        if (displayRotation == Surface.ROTATION_180) {
            matrix.postRotate(180f, cx, cy)
        }
    }
    textureView.setTransform(matrix)
}

@Composable
fun BroadcastScreen(
    cameraStreamer: Camera2Streamer,
    micStreamer: AudioInputStreamer,
    hasCameraPermission: Boolean
) {
    val context = LocalContext.current
    val configuration = LocalConfiguration.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    val appVersion = remember { LanVidNative.nativeGetVersion() }
    val simdInfo = remember { LanVidNative.nativeGetSimdInfo() }

    var isAppResumed by remember { mutableStateOf(true) }
    var isTogglingLive by remember { mutableStateOf(false) }

    val prefs = remember { context.getSharedPreferences("lanvid_camera", Context.MODE_PRIVATE) }
    val cameraId = remember {
        val stored = prefs.getInt("cameraId", 0)
        if (stored > 0) stored else Random.nextInt(1000, 999_999).also {
            prefs.edit().putInt("cameraId", it).apply()
        }
    }
    val discovery = remember { LanPeerDiscovery(context) }
    DisposableEffect(lifecycleOwner) {
        val obs = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> {
                    isAppResumed = true
                    discovery.start()
                }
                Lifecycle.Event.ON_PAUSE -> {
                    isAppResumed = false
                    discovery.stop()
                    cameraStreamer.stop()
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(obs)
        discovery.start()
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(obs)
            discovery.release()
        }
    }

    var ui by remember {
        mutableStateOf(
            HudUiState(
                destIp = prefs.getString("destIp", "") ?: "",
                destPort = prefs.getInt("destPort", 5006),
                destName = prefs.getString("destName", "") ?: ""
            )
        )
    }
    var userManuallyPickedIp by remember { mutableStateOf(false) }
    var selectedTool by remember { mutableStateOf<HudTool?>(null) }
    var showManualDeck by remember { mutableStateOf(false) }
    var showEncoderDrawer by remember { mutableStateOf(false) }
    var hudHidden by remember { mutableStateOf(false) }

    val lanPeers = discovery.peers
    val scanning = discovery.scanning
    var txStats by remember { mutableStateOf(SenderStats()) }
    var rxStats by remember { mutableStateOf(ReceiverStats()) }
    var traffic by remember { mutableStateOf(TrafficState()) }
    var battery by remember { mutableIntStateOf(-1) }
    var batteryTempC by remember { mutableIntStateOf(34) }
    var proState by remember { mutableStateOf(ProCameraState()) }

    fun pushLocalConfigSync(nextUi: HudUiState, nextPro: ProCameraState) {
        if (!nextUi.senderActive) return
        val resIdx = when (nextUi.resolution) {
            ResolutionPreset.HD_720 -> 1
            ResolutionPreset.FHD_1080 -> 2
            ResolutionPreset.UHD_4K -> 3
        }
        val fpsInt = nextPro.fps.toIntOrNull() ?: 60
        scope.launch(Dispatchers.IO) {
            LanVidNative.nativeSendVideoConfigSync(
                camNum = nextPro.assignedCamNumber,
                camTag = nextPro.badgeDisplayText,
                resPreset = resIdx,
                fps = fpsInt,
                codec = nextUi.codec.id,
                bitrateKbps = (nextUi.bitrateMbps * 1000).toInt(),
                delayMs = nextPro.syncedDelayMs,
                fixedDelay = nextPro.syncedFixedDelay,
                lens = if (nextUi.source == CaptureSource.FRONT_CAM) 3 else 0
            )
        }
    }

    fun saveTargetPc(ip: String, port: Int, name: String) {
        prefs.edit()
            .putString("destIp", ip)
            .putInt("destPort", port)
            .putString("destName", name)
            .apply()
    }

    // Auto-select the best discovered [VIDEO] or [STUDIO] PC automatically so the user just taps GO LIVE!
    LaunchedEffect(lanPeers.size, ui.senderActive) {
        if (ui.senderActive || lanPeers.isEmpty()) return@LaunchedEffect
        val preferred = lanPeers.firstOrNull { it.role.equals("video", ignoreCase = true) }
            ?: lanPeers.firstOrNull { !it.role.equals("audio", ignoreCase = true) }
            ?: return@LaunchedEffect
        val currentStillOnline = lanPeers.any { it.ip == ui.destIp && it.port == ui.destPort }
        if (ui.destIp.isBlank() || ui.destIp == "192.168.1.100" || (!userManuallyPickedIp && !currentStillOnline)) {
            ui = ui.copy(destIp = preferred.ip, destPort = preferred.port, destName = preferred.name)
            saveTargetPc(preferred.ip, preferred.port, preferred.name)
        }
    }

    var cameraTextureView by remember { mutableStateOf<TextureView?>(null) }
    var cameraSurfaceTexture by remember { mutableStateOf<SurfaceTexture?>(null) }
    var cameraPreviewSurface by remember { mutableStateOf<Surface?>(null) }
    var viewW by remember { mutableIntStateOf(0) }
    var viewH by remember { mutableIntStateOf(0) }
    var camBufW by remember { mutableIntStateOf(1280) }
    var camBufH by remember { mutableIntStateOf(720) }
    var camOrientedW by remember { mutableIntStateOf(720) }
    var camOrientedH by remember { mutableIntStateOf(1280) }

    val displayRotation = remember(configuration.orientation, viewW, viewH) {
        getDisplayRotation(context)
    }

    // Re-apply aspect-preserving TextureView transform whenever geometry, crop mode, or rotation changes.
    LaunchedEffect(
        cameraTextureView,
        cameraSurfaceTexture,
        viewW,
        viewH,
        camBufW,
        camBufH,
        camOrientedW,
        camOrientedH,
        displayRotation,
        ui.previewCropToFill
    ) {
        val tv = cameraTextureView ?: return@LaunchedEffect
        cameraSurfaceTexture?.setDefaultBufferSize(camBufW, camBufH)
        configureCameraTextureTransform(
            textureView = tv,
            viewW = viewW,
            viewH = viewH,
            bufW = camBufW,
            bufH = camBufH,
            orientedW = camOrientedW,
            orientedH = camOrientedH,
            displayRotation = displayRotation,
            cropToFill = ui.previewCropToFill
        )
    }

    // Studio Tally & Duplex Intercom connection + 50 Hz poll + full-duplex voice I/O (ALL off Main UI thread!)
    LaunchedEffect(ui.mode, ui.senderActive, ui.destIp) {
        if (ui.mode == BroadcastMode.SENDER && ui.senderActive && ui.destIp.isNotBlank()) {
            val destIp = ui.destIp
            val label = proState.cameraLabel
            val connected = withContext(Dispatchers.IO) {
                val ok = LanVidNative.nativeTallyConnect(destIp, 5022, cameraId, label)
                if (ok) {
                    LanVidNative.nativeIntercomSetState(proState.pttHeld || proState.latchTalk, proState.ifbMuted, proState.ifbGain, 0f)
                }
                ok
            }
            if (!connected) return@LaunchedEffect

            // Full-duplex Intercom audio job: IFB playback (AudioTrack) + PTT mic capture (AudioRecord) @ 48 kHz mono Float32
            val intercomAudioJob = launch(Dispatchers.IO) {
                val sr = 48000
                val framesPerChunk = 240 // 5 ms
                val trackBuf = AudioTrack.getMinBufferSize(sr, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT)
                    .coerceAtLeast(framesPerChunk * 4 * 4)
                val track = runCatching {
                    AudioTrack.Builder()
                        .setAudioAttributes(
                            AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                                .build()
                        )
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                                .setSampleRate(sr)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build()
                        )
                        .setBufferSizeInBytes(trackBuf)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build()
                }.getOrNull()?.takeIf { it.state == AudioTrack.STATE_INITIALIZED }
                runCatching { track?.play() }

                val hasMicPerm = context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
                val recBuf = AudioRecord.getMinBufferSize(sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT)
                    .coerceAtLeast(framesPerChunk * 4 * 4)
                val recorder = if (hasMicPerm) {
                    runCatching {
                        AudioRecord.Builder()
                            .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                            .setAudioFormat(
                                AudioFormat.Builder()
                                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                                    .setSampleRate(sr)
                                    .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                                    .build()
                            )
                            .setBufferSizeInBytes(recBuf)
                            .build()
                    }.getOrNull()?.takeIf { it.state == AudioRecord.STATE_INITIALIZED }
                } else null
                runCatching { recorder?.startRecording() }

                val rxJob = launch(Dispatchers.IO) {
                    val rxPcm = FloatArray(framesPerChunk)
                    while (isActive) {
                        val got = LanVidNative.nativeIntercomPullRx(rxPcm, framesPerChunk)
                        if (got > 0 && track != null) {
                            track.write(rxPcm, 0, got, AudioTrack.WRITE_BLOCKING)
                        } else {
                            delay(5)
                        }
                    }
                }
                val txJob = launch(Dispatchers.IO) {
                    if (recorder == null) return@launch
                    val txPcm = FloatArray(framesPerChunk)
                    while (isActive) {
                        val read = recorder.read(txPcm, 0, framesPerChunk, AudioRecord.READ_BLOCKING)
                        if (read < 0) break
                        if (read > 0 && (proState.pttHeld || proState.latchTalk)) {
                            LanVidNative.nativeIntercomPushMic(txPcm, read)
                        }
                    }
                }
                try {
                    rxJob.join()
                    txJob.join()
                } finally {
                    rxJob.cancel()
                    txJob.cancel()
                    runCatching { recorder?.stop() }
                    runCatching { recorder?.release() }
                    runCatching { track?.stop() }
                    runCatching { track?.release() }
                }
            }

            try {
                var tickCount = 0
                while (true) {
                    delay(20)
                    tickCount++
                    val bat = battery.coerceIn(0, 100)
                    val temp = batteryTempC
                    val raw = withContext(Dispatchers.IO) {
                        LanVidNative.nativeTallyPollJson(bat, temp)
                    }
                    if (tickCount % 5 != 0) continue // UI state refresh at 10 Hz; network poll at 50 Hz
                    if (raw.isNotEmpty()) {
                        runCatching {
                            val obj = JSONObject(raw)
                            val tVal = obj.optInt("tally", obj.optInt("state", 0))
                            val tState = when (tVal) {
                                2 -> TallyStatus.PROGRAM
                                1 -> TallyStatus.PREVIEW
                                else -> TallyStatus.STANDBY
                            }
                            val newNum = obj.optInt("assignedCamNumber", proState.assignedCamNumber)
                            val newTag = obj.optString("assignedCamTag", proState.assignedCamTag)
                            val syncRev = obj.optLong("syncRev", 0L)
                            val synced = obj.optBoolean("configSynced", false)
                            val delayMs = obj.optInt("syncDelayMs", proState.syncedDelayMs).coerceIn(5, 250)
                            val fixedDelay = obj.optBoolean("syncFixedDelay", proState.syncedFixedDelay)
                            val nextLabel = newTag.ifBlank { obj.optString("label", proState.cameraLabel) }
                                .ifBlank { proState.cameraLabel }

                            // Capture previous sync revision BEFORE updating proState!
                            val prevSyncRev = proState.lastSyncRev
                            proState = proState.copy(
                                tally = tState,
                                remoteRec = obj.optBoolean("remoteRec", obj.optInt("rec", 0) != 0),
                                directorSpeaking = obj.optBoolean("directorSpeaking", obj.optInt("director_speaking", 0) != 0),
                                duckingGain = obj.optDouble("ducking", obj.optDouble("duck", 1.0)).toFloat(),
                                cameraLabel = nextLabel,
                                assignedCamNumber = newNum,
                                assignedCamTag = newTag,
                                configSynced = synced,
                                syncedDelayMs = delayMs,
                                syncedFixedDelay = fixedDelay,
                                lastSyncRev = maxOf(prevSyncRev, syncRev)
                            )

                            if (synced && syncRev > prevSyncRev) {
                                val syncRes = obj.optInt("syncRes", -1)
                                val syncCodec = obj.optInt("syncCodec", -1)
                                val syncBitrateKbps = obj.optInt("syncBitrateKbps", 0)
                                val syncFps = obj.optInt("syncFps", 0)
                                val syncLens = obj.optInt("syncLens", -1)

                                val targetRes = when (syncRes) {
                                    0, 1 -> ResolutionPreset.HD_720
                                    2 -> ResolutionPreset.FHD_1080
                                    3 -> ResolutionPreset.UHD_4K
                                    else -> ui.resolution
                                }
                                val targetCodec = when (syncCodec) {
                                    0 -> VideoCodecOption.DCT
                                    1 -> VideoCodecOption.H264
                                    2 -> VideoCodecOption.H265
                                    else -> ui.codec
                                }
                                val targetBitrateMbps = if (syncBitrateKbps >= 1000) {
                                    (syncBitrateKbps / 1000.0).coerceIn(1.0, 150.0)
                                } else ui.bitrateMbps
                                val targetSource = if (syncLens == 3) CaptureSource.FRONT_CAM
                                else if (syncLens in 0..2 && ui.source == CaptureSource.FRONT_CAM) CaptureSource.BACK_CAM
                                else ui.source

                                val prevFps = proState.fps.toIntOrNull() ?: 60
                                val targetFps = if (syncFps >= 15) syncFps else prevFps

                                val onlyBitrateChanged = (targetRes == ui.resolution &&
                                    targetCodec == ui.codec &&
                                    targetFps == prevFps &&
                                    targetSource == ui.source &&
                                    targetBitrateMbps != ui.bitrateMbps)

                                val needRestartSender = (targetRes != ui.resolution || targetCodec != ui.codec || targetFps != prevFps)
                                ui = ui.copy(
                                    resolution = targetRes,
                                    codec = targetCodec,
                                    bitrateMbps = targetBitrateMbps,
                                    source = targetSource
                                )
                                proState = proState.copy(
                                    resolution = when (targetRes) {
                                        ResolutionPreset.UHD_4K -> "4K UHD"
                                        ResolutionPreset.FHD_1080 -> "1080p"
                                        ResolutionPreset.HD_720 -> "720p"
                                    },
                                    codec = when (targetCodec) {
                                        VideoCodecOption.H265 -> "H.265"
                                        VideoCodecOption.H264 -> "H.264"
                                        VideoCodecOption.DCT -> "DCT"
                                    },
                                    bitrateMbps = targetBitrateMbps.toFloat(),
                                    fps = targetFps.toString(),
                                    lens = if (syncLens == 3) "Front" else if (syncLens == 1) "0.5x" else if (syncLens == 2) "2x" else "1x"
                                )
                                if (onlyBitrateChanged) {
                                    withContext(Dispatchers.IO) {
                                        LanVidNative.nativeSetSenderBitrateKbps((targetBitrateMbps * 1000).toInt())
                                    }
                                } else if (needRestartSender && ui.senderActive) {
                                    val encW = targetRes.width
                                    val encH = targetRes.height
                                    withContext(Dispatchers.IO) {
                                        LanVidNative.nativeStartSender(
                                            ui.destIp, ui.destPort, encW, encH, targetFps,
                                            ui.quality, ui.fecGroup, ui.adaptive, targetCodec.id,
                                            (targetBitrateMbps * 1000).toInt()
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            } finally {
                intercomAudioJob.cancel()
                withContext(NonCancellable + Dispatchers.IO) { LanVidNative.nativeTallyDisconnect() }
            }
        } else {
            withContext(Dispatchers.IO) { LanVidNative.nativeTallyDisconnect() }
            proState = proState.copy(tally = TallyStatus.STANDBY, directorSpeaking = false, duckingGain = 1.0f)
        }
    }

    // Restart live Camera2 viewfinder whenever lens, resolution, fps, surface, orientation, resume state, or mode changes
    LaunchedEffect(
        isAppResumed,
        hasCameraPermission,
        ui.mode,
        ui.source,
        ui.resolution,
        proState.fps,
        displayRotation,
        cameraPreviewSurface
    ) {
        val surface = cameraPreviewSurface
        if (isAppResumed &&
            ui.mode == BroadcastMode.SENDER &&
            ui.source != CaptureSource.SCREEN &&
            hasCameraPermission &&
            surface != null &&
            surface.isValid
        ) {
            cameraStreamer.start(
                width = ui.resolution.width,
                height = ui.resolution.height,
                frontFacing = ui.source == CaptureSource.FRONT_CAM,
                targetFps = proState.fps.toIntOrNull() ?: 60,
                displayRotation = displayRotation,
                previewSurface = surface,
                streamEnabled = ui.senderActive,
                onPreviewConfigured = { bW, bH, oW, oH, _ ->
                    camBufW = bW
                    camBufH = bH
                    camOrientedW = oW
                    camOrientedH = oH
                    cameraSurfaceTexture?.setDefaultBufferSize(bW, bH)
                    cameraTextureView?.post {
                        val tv = cameraTextureView ?: return@post
                        configureCameraTextureTransform(
                            textureView = tv,
                            viewW = tv.width,
                            viewH = tv.height,
                            bufW = bW,
                            bufH = bH,
                            orientedW = oW,
                            orientedH = oH,
                            displayRotation = displayRotation,
                            cropToFill = ui.previewCropToFill
                        )
                    }
                }
            ) { err ->
                ui = ui.copy(statusMessage = err)
            }
        } else {
            cameraStreamer.stop()
        }
    }

    // Screen capture MediaProjection launcher
    val projectionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { res ->
        if (res.resultCode == Activity.RESULT_OK && res.data != null) {
            val dm: DisplayMetrics = context.resources.displayMetrics
            val capW = ui.resolution.width.coerceAtMost(1280)
            val capH = ((capW * dm.heightPixels / dm.widthPixels.coerceAtLeast(1)) and 0x7FFE).coerceIn(480, 1440)
            val snap = ui
            val resCode = res.resultCode
            val resData = res.data
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    LanVidNative.nativeStartSender(
                        snap.destIp,
                        snap.destPort,
                        capW,
                        capH,
                        proState.fps.toIntOrNull() ?: 60,
                        snap.quality,
                        snap.fecGroup,
                        snap.adaptive,
                        snap.codec.id,
                        (snap.bitrateMbps * 1000).toInt()
                    )
                }
                if (ok) {
                    val svcIntent = Intent(context, ScreenCaptureService::class.java).apply {
                        action = ScreenCaptureService.ACTION_START
                        putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, resCode)
                        putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, resData)
                        putExtra(ScreenCaptureService.EXTRA_WIDTH, capW)
                        putExtra(ScreenCaptureService.EXTRA_HEIGHT, capH)
                        putExtra(ScreenCaptureService.EXTRA_DPI, dm.densityDpi)
                        putExtra(ScreenCaptureService.EXTRA_AUDIO_MODE, snap.audioSource.id)
                    }
                    context.startForegroundService(svcIntent)
                    ui = ui.copy(
                        senderActive = true,
                        statusMessage = "Screen cast -> ${snap.destIp}:${snap.destPort}"
                    )
                } else {
                    ui = ui.copy(statusMessage = "Не вдалося запустити трансляцію екрана")
                }
            }
        } else {
            ui = ui.copy(statusMessage = "Screen capture permission denied")
        }
    }

    // Battery level & real device temperature monitor
    LaunchedEffect(Unit) {
        while (true) {
            battery = context.getSystemService(BatteryManager::class.java)
                ?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
            val batIntent = runCatching {
                context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            }.getOrNull()
            val tenthsC = batIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 340) ?: 340
            if (tenthsC > 0) batteryTempC = (tenthsC / 10).coerceIn(0, 100)
            delay(10_000)
        }
    }

    // 500 ms telemetry & traffic sparkline loop (discovery runs continuously in LanPeerDiscovery)
    LaunchedEffect(Unit) {
        var liveStartMs = 0L
        var wasLive = false
        while (true) {
            delay(500)
            val (txS, rxS) = withContext(Dispatchers.IO) {
                LanVidNative.senderStats() to LanVidNative.receiverStats()
            }
            txStats = txS
            rxStats = rxS
            val liveNow = ui.isLive
            if (liveNow && !wasLive) {
                liveStartMs = System.currentTimeMillis()
                traffic = TrafficState(live = true)
            } else if (!liveNow && wasLive) {
                traffic = traffic.copy(live = false, instantMbps = 0.0)
            }
            wasLive = liveNow
            if (liveNow) {
                val instMbps = if (ui.mode == BroadcastMode.SENDER) txStats.mbps else rxStats.mbps
                val hist = (traffic.historyMbps + instMbps.toFloat()).takeLast(30)
                traffic = traffic.copy(
                    live = true,
                    elapsedSec = ((System.currentTimeMillis() - liveStartMs) / 1000L).coerceAtLeast(0L),
                    instantMbps = instMbps,
                    peakMbps = maxOf(traffic.peakMbps, instMbps),
                    totalBytes = traffic.totalBytes + (instMbps * 1_000_000.0 / 8.0) * 0.5,
                    historyMbps = hist
                )
            }
        }
    }

    fun stopAll() {
        cameraStreamer.setStreamingEnabled(false)
        ui = ui.copy(senderActive = false, receiverActive = false)
        scope.launch(Dispatchers.IO) {
            micStreamer.stop()
            if (ScreenCaptureService.isRunning) {
                context.stopService(Intent(context, ScreenCaptureService::class.java))
            }
            LanVidNative.nativeStopSender()
            LanVidNative.nativeStopReceiver()
            LanVidNative.nativeTallyDisconnect()
        }
    }

    fun toggleLive(overridePeer: LanPeer? = null) {
        if (isTogglingLive) return
        if (ui.mode == BroadcastMode.SENDER) {
            if (!ui.senderActive) {
                // Auto-resolve best discovered PC if none selected yet
                var targetIp = overridePeer?.ip ?: ui.destIp
                var targetPort = overridePeer?.port ?: ui.destPort
                var targetName = overridePeer?.name ?: ui.destName
                if (targetIp.isBlank()) {
                    val auto = lanPeers.firstOrNull { it.role.equals("video", ignoreCase = true) }
                        ?: lanPeers.firstOrNull { !it.role.equals("audio", ignoreCase = true) }
                    if (auto != null) {
                        targetIp = auto.ip
                        targetPort = auto.port
                        targetName = auto.name
                    } else {
                        selectedTool = HudTool.NETWORK
                        discovery.refreshNow()
                        ui = ui.copy(statusMessage = "ПК не знайдено — відкрийте LanVid Video на ПК у тій же Wi-Fi мережі")
                        return
                    }
                }
                if (targetIp != ui.destIp || targetPort != ui.destPort || targetName != ui.destName) {
                    ui = ui.copy(destIp = targetIp, destPort = targetPort, destName = targetName)
                }
                saveTargetPc(targetIp, targetPort, targetName)

                if (ui.source == CaptureSource.SCREEN) {
                    val mpm = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                    projectionLauncher.launch(mpm.createScreenCaptureIntent())
                } else {
                    isTogglingLive = true
                    val encW = cameraStreamer.outputWidth.takeIf { it > 0 } ?: ui.resolution.width
                    val encH = cameraStreamer.outputHeight.takeIf { it > 0 } ?: ui.resolution.height
                    val snap = ui
                    scope.launch {
                        val ok = withContext(Dispatchers.IO) {
                            LanVidNative.nativeStartSender(
                                targetIp,
                                targetPort,
                                encW,
                                encH,
                                proState.fps.toIntOrNull() ?: 60,
                                snap.quality,
                                snap.fecGroup,
                                snap.adaptive,
                                snap.codec.id,
                                (snap.bitrateMbps * 1000).toInt()
                            )
                        }
                        if (ok) {
                            cameraStreamer.setStreamingEnabled(true)
                            if (snap.audioSource == AudioSourceOption.MIC) {
                                withContext(Dispatchers.IO) { micStreamer.start(null) }
                            }
                            val lbl = if (targetName.isNotBlank()) "$targetName ($targetIp:$targetPort)" else "$targetIp:$targetPort"
                            ui = ui.copy(
                                senderActive = true,
                                statusMessage = "LIVE -> $lbl · ${encW}×${encH}"
                            )
                        } else {
                            ui = ui.copy(statusMessage = "Не вдалося запустити передачу на $targetIp:$targetPort")
                        }
                        isTogglingLive = false
                    }
                }
            } else {
                stopAll()
                ui = ui.copy(statusMessage = "Stream stopped")
            }
        } else {
            if (!ui.receiverActive) {
                isTogglingLive = true
                val snap = ui
                scope.launch {
                    val ok = withContext(Dispatchers.IO) {
                        val started = LanVidNative.nativeStartReceiver(snap.listenPort, snap.audioLatencyMs.toDouble(), Build.MODEL)
                        if (started) {
                            LanVidNative.nativeSetVolume(snap.volume)
                            LanVidNative.nativeSetAudioLatencyMs(snap.audioLatencyMs)
                        }
                        started
                    }
                    ui = if (ok) {
                        ui.copy(
                            receiverActive = true,
                            statusMessage = "Listening on UDP :${snap.listenPort} (AAudio + Discovery)"
                        )
                    } else {
                        ui.copy(statusMessage = "Failed to bind UDP :${snap.listenPort}")
                    }
                    isTogglingLive = false
                }
            } else {
                stopAll()
                ui = ui.copy(statusMessage = "Receiver stopped")
            }
        }
    }

    val tally = when {
        ui.mode == BroadcastMode.SENDER && ui.senderActive -> when (proState.tally) {
            TallyStatus.PROGRAM -> TallyVisual.PROGRAM
            TallyStatus.PREVIEW -> TallyVisual.PREVIEW
            TallyStatus.STANDBY -> TallyVisual.OFF
        }
        ui.receiverActive -> TallyVisual.PREVIEW
        else -> TallyVisual.OFF
    }

    val safeInsets = WindowInsets.safeDrawing.union(WindowInsets.displayCutout)

    Box(
        Modifier
            .fillMaxSize()
            .background(Bm.bg)
    ) {
        // 1. Full-bleed aspect-ratio-preserving live preview (TextureView with Crop/Fit matrix in Camera mode)
        if (ui.mode == BroadcastMode.RECEIVER) {
            AndroidView(
                factory = { ctx ->
                    SurfaceView(ctx).apply {
                        holder.addCallback(object : SurfaceHolder.Callback {
                            override fun surfaceCreated(h: SurfaceHolder) {
                                LanVidNative.nativeSetReceiverSurface(h.surface)
                            }
                            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {
                                LanVidNative.nativeSetReceiverSurface(h.surface)
                            }
                            override fun surfaceDestroyed(h: SurfaceHolder) {
                                LanVidNative.nativeSetReceiverSurface(null)
                            }
                        })
                    }
                },
                modifier = Modifier.fillMaxSize()
            )
        } else if (ui.source != CaptureSource.SCREEN) {
            AndroidView(
                factory = { ctx ->
                    TextureView(ctx).apply {
                        cameraTextureView = this
                        surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                                viewW = w
                                viewH = h
                                cameraSurfaceTexture = st
                                st.setDefaultBufferSize(camBufW, camBufH)
                                cameraPreviewSurface?.release()
                                cameraPreviewSurface = Surface(st)
                            }

                            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                                viewW = w
                                viewH = h
                            }

                            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                                val oldSurface = cameraPreviewSurface
                                cameraPreviewSurface = null
                                cameraSurfaceTexture = null
                                cameraStreamer.stop {
                                    runCatching { oldSurface?.release() }
                                }
                                return true
                            }

                            override fun onSurfaceTextureUpdated(st: SurfaceTexture) = Unit
                        }
                    }
                },
                modifier = Modifier.fillMaxSize()
            )
        } else {
            Box(Modifier.fillMaxSize().background(Color(0xFF0A0A0A)), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("SCREEN CAPTURE MODE", style = Bm.mono(16.sp, FontWeight.Bold), color = Bm.accent)
                    Text(
                        if (ui.senderActive) "Streaming MediaProjection -> ${ui.destIp}:${ui.destPort}"
                        else "Press GO LIVE to share Android screen",
                        style = Bm.mono(11.sp),
                        color = Bm.dim
                    )
                }
            }
        }

        // 2. Operator-only frame guides overlay (rule of thirds + 90% broadcast safe area)
        if (!hudHidden && ui.frameGuides) {
            FrameGuidesOverlay(Modifier.fillMaxSize())
        }

        // 3. Broadcast Tally outer border
        TallyBorder(tally)

        // 4. Unified non-overlapping HUD Overlay (respects DisplayCutout + status/navigation bars)
        if (hudHidden) {
            Box(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(safeInsets)
                    .padding(12.dp)
            ) {
                TallyBadge(tally, Modifier.align(Alignment.TopStart))
                if (ui.mode == BroadcastMode.SENDER) {
                    CameraNumberBadge(
                        state = proState,
                        onOpenNetwork = {
                            hudHidden = false
                            selectedTool = HudTool.NETWORK
                        },
                        modifier = Modifier.align(Alignment.TopEnd)
                    )
                }
                if (ui.showTimer || ui.showTraffic) {
                    TrafficAndTimerCard(
                        traffic = traffic,
                        isSender = ui.mode == BroadcastMode.SENDER,
                        showTimer = ui.showTimer,
                        showTraffic = ui.showTraffic,
                        modifier = Modifier.align(Alignment.BottomStart)
                    )
                }
                Text(
                    "◉ HUD",
                    style = Bm.mono(12.sp),
                    color = Color.White.copy(alpha = 0.8f),
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .background(Color.Black.copy(alpha = 0.5f), CircleShape)
                        .padding(horizontal = 12.dp, vertical = 8.dp)
                        .clickableNoRipple { hudHidden = false }
                )
            }
        } else {
            Column(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(safeInsets)
            ) {
                // 4a. Top Bar (status, tally, timer, bitrate, format, battery, HUD/mode toggle)
                TopHudBar(
                    ui = ui,
                    fpsLabel = proState.fps,
                    tally = tally,
                    traffic = traffic,
                    txStats = txStats,
                    rxStats = rxStats,
                    battery = battery,
                    onHideHud = {
                        selectedTool = null
                        showManualDeck = false
                        showEncoderDrawer = false
                        hudHidden = true
                    },
                    onSwitchMode = {
                        stopAll()
                        selectedTool = null
                        showManualDeck = false
                        showEncoderDrawer = false
                        ui = ui.copy(mode = if (ui.mode == BroadcastMode.SENDER) BroadcastMode.RECEIVER else BroadcastMode.SENDER)
                    }
                )

                // 4b. Sub-Top Bar (CAM label + MANUAL / ENC drawers) — strictly below TopHudBar
                if (ui.mode == BroadcastMode.SENDER) {
                    ProSubTopBar(
                        state = proState,
                        showManualDeck = showManualDeck,
                        showEncoderDrawer = showEncoderDrawer,
                        onToggleManual = {
                            showManualDeck = !showManualDeck
                            if (showManualDeck) {
                                showEncoderDrawer = false
                                selectedTool = null
                            }
                        },
                        onToggleEncoder = {
                            showEncoderDrawer = !showEncoderDrawer
                            if (showEncoderDrawer) {
                                showManualDeck = false
                                selectedTool = null
                            }
                        }
                    )
                }

                // 4c. Center Viewfinder Zone (banners + slide-out MANUAL / ENC / Tool drawers)
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxWidth()
                        .then(
                            if (selectedTool != null || showManualDeck || showEncoderDrawer) {
                                Modifier.clickableNoRipple {
                                    selectedTool = null
                                    showManualDeck = false
                                    showEncoderDrawer = false
                                }
                            } else Modifier
                        )
                ) {
                    if (ui.mode == BroadcastMode.SENDER) {
                        CameraNumberBadge(
                            state = proState,
                            onOpenNetwork = { selectedTool = HudTool.NETWORK },
                            modifier = Modifier
                                .align(Alignment.TopEnd)
                                .padding(horizontal = 10.dp, vertical = 4.dp)
                        )
                    }

                    // Dismissible status / error banner
                    ui.statusMessage?.let { msg ->
                        Box(
                            Modifier
                                .align(Alignment.TopCenter)
                                .fillMaxWidth()
                                .padding(horizontal = 12.dp, vertical = 4.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                "$msg   ✕",
                                style = Bm.mono(11.sp),
                                color = Color.White,
                                maxLines = 2,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier
                                    .background(Bm.panelHi.copy(alpha = 0.92f), RoundedCornerShape(14.dp))
                                    .clickableNoRipple { ui = ui.copy(statusMessage = null) }
                                    .padding(horizontal = 12.dp, vertical = 6.dp)
                            )
                        }
                    }

                    if (ui.mode == BroadcastMode.SENDER && showManualDeck) {
                        ProManualDeckDrawer(
                            state = proState,
                            onStateChange = { next ->
                                val prev = proState
                                proState = next
                                var nextUi = ui
                                if (next.lens != prev.lens) {
                                    val nextSource = if (next.lens == "Front") CaptureSource.FRONT_CAM else CaptureSource.BACK_CAM
                                    if (ui.source != nextSource) {
                                        nextUi = ui.copy(source = nextSource)
                                        ui = nextUi
                                    }
                                    pushLocalConfigSync(nextUi, next)
                                }
                                val expChanged = next.iso != prev.iso || next.shutterAngleDeg != prev.shutterAngleDeg
                                val wbChanged = next.wbKelvin != prev.wbKelvin
                                val focusChanged = next.focusDist != prev.focusDist
                                if (expChanged || wbChanged || focusChanged) {
                                    val nextLocked = next.copy(
                                        shutterLocked = next.shutterLocked || expChanged,
                                        isoLocked = next.isoLocked || expChanged,
                                        wbLocked = next.wbLocked || wbChanged,
                                        focusLocked = next.focusLocked || focusChanged
                                    )
                                    proState = nextLocked
                                    cameraStreamer.applyManualControls(
                                        manualExposure = nextLocked.isoLocked || nextLocked.shutterLocked,
                                        iso = nextLocked.iso,
                                        shutterAngleDeg = nextLocked.shutterAngleDeg,
                                        fps = nextLocked.fps.toIntOrNull() ?: 60,
                                        manualWb = nextLocked.wbLocked,
                                        wbKelvin = nextLocked.wbKelvin,
                                        manualFocus = nextLocked.focusLocked,
                                        focusNormalized = nextLocked.focusDist
                                    )
                                }
                            },
                            modifier = Modifier.align(Alignment.CenterStart)
                        )
                    }

                    if (ui.mode == BroadcastMode.SENDER && showEncoderDrawer) {
                        ProEncoderDrawer(
                            state = proState,
                            onStateChange = { next ->
                                val prevUi = ui
                                val prevFps = proState.fps
                                proState = next
                                val nextRes = when (next.resolution) {
                                    "4K UHD" -> ResolutionPreset.UHD_4K
                                    "1080p" -> ResolutionPreset.FHD_1080
                                    else -> ResolutionPreset.HD_720
                                }
                                val nextCodec = when (next.codec) {
                                    "H.265" -> VideoCodecOption.H265
                                    "H.264" -> VideoCodecOption.H264
                                    else -> VideoCodecOption.DCT
                                }
                                val nextMbps = next.bitrateMbps.toDouble().coerceIn(1.0, 150.0)
                                val nextUi = ui.copy(
                                    resolution = nextRes,
                                    codec = nextCodec,
                                    bitrateMbps = nextMbps
                                )
                                ui = nextUi
                                pushLocalConfigSync(nextUi, next)
                                if (nextUi.senderActive) {
                                    val needRestart = (nextRes != prevUi.resolution || nextCodec != prevUi.codec || next.fps != prevFps)
                                    val fpsVal = next.fps.toIntOrNull() ?: 60
                                    scope.launch(Dispatchers.IO) {
                                        if (needRestart) {
                                            LanVidNative.nativeStartSender(
                                                nextUi.destIp, nextUi.destPort, nextRes.width, nextRes.height,
                                                fpsVal, nextUi.quality, nextUi.fecGroup, nextUi.adaptive,
                                                nextCodec.id, (nextMbps * 1000).toInt()
                                            )
                                        } else if (nextMbps != prevUi.bitrateMbps) {
                                            LanVidNative.nativeSetSenderBitrateKbps((nextMbps * 1000).toInt())
                                        }
                                    }
                                }
                            },
                            modifier = Modifier.align(Alignment.CenterEnd)
                        )
                    }

                    selectedTool?.let { tool ->
                        ToolPanel(
                            tool = tool,
                            ui = ui,
                            appVersion = appVersion,
                            simdInfo = simdInfo,
                            txStats = txStats,
                            rxStats = rxStats,
                            lanPeers = lanPeers,
                            scanning = scanning,
                            onScanLan = { discovery.refreshNow() },
                            onSelectPeerAndGoLive = { peer ->
                                userManuallyPickedIp = true
                                selectedTool = null
                                toggleLive(overridePeer = peer)
                            },
                            onUpdate = { next ->
                                val prevUi = ui
                                if (next.destIp != ui.destIp || next.destPort != ui.destPort) {
                                    userManuallyPickedIp = true
                                    saveTargetPc(next.destIp, next.destPort, next.destName)
                                }
                                ui = next
                                if (next.resolution != prevUi.resolution || next.codec != prevUi.codec || next.bitrateMbps != prevUi.bitrateMbps) {
                                    val syncedPro = proState.copy(
                                        resolution = when (next.resolution) {
                                            ResolutionPreset.UHD_4K -> "4K UHD"
                                            ResolutionPreset.FHD_1080 -> "1080p"
                                            ResolutionPreset.HD_720 -> "720p"
                                        },
                                        codec = when (next.codec) {
                                            VideoCodecOption.H265 -> "H.265"
                                            VideoCodecOption.H264 -> "H.264"
                                            VideoCodecOption.DCT -> "DCT"
                                        },
                                        bitrateMbps = next.bitrateMbps.toFloat()
                                    )
                                    proState = syncedPro
                                    pushLocalConfigSync(next, syncedPro)
                                    if (next.senderActive) {
                                        val needRestart = (next.resolution != prevUi.resolution || next.codec != prevUi.codec)
                                        val fpsVal = syncedPro.fps.toIntOrNull() ?: 60
                                        scope.launch(Dispatchers.IO) {
                                            if (needRestart) {
                                                LanVidNative.nativeStartSender(
                                                    next.destIp, next.destPort, next.resolution.width, next.resolution.height,
                                                    fpsVal, next.quality, next.fecGroup, next.adaptive,
                                                    next.codec.id, (next.bitrateMbps * 1000).toInt()
                                                )
                                            } else if (next.bitrateMbps != prevUi.bitrateMbps) {
                                                LanVidNative.nativeSetSenderBitrateKbps((next.bitrateMbps * 1000).toInt())
                                            }
                                        }
                                    }
                                }
                            },
                            onSwitchMode = { next ->
                                if (next != ui.mode) {
                                    stopAll()
                                    ui = ui.copy(mode = next)
                                }
                            },
                            onVolumeChange = { v ->
                                ui = ui.copy(volume = v)
                                LanVidNative.nativeSetVolume(v)
                            },
                            onAudioLatencyChange = { ms ->
                                ui = ui.copy(audioLatencyMs = ms)
                                if (ui.receiverActive) LanVidNative.nativeSetAudioLatencyMs(ms)
                            },
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .padding(horizontal = 10.dp, vertical = 6.dp)
                        )
                    }
                }

                // 4d. Sequential Bottom Stack: Intercom Strip -> Telemetry Row -> Bottom Action Deck (0 overlap!)
                Column(
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    if (ui.mode == BroadcastMode.SENDER) {
                        ProIntercomStrip(
                            state = proState,
                            onStateChange = { proState = it },
                            onIntercomChanged = { talk, listenMuted, rxGain ->
                                scope.launch(Dispatchers.IO) {
                                    LanVidNative.nativeIntercomSetState(talk, listenMuted, rxGain, 0f)
                                }
                            }
                        )
                    }

                    if (ui.showTimer || ui.showTraffic) {
                        TrafficAndTimerCard(
                            traffic = traffic,
                            isSender = ui.mode == BroadcastMode.SENDER,
                            showTimer = ui.showTimer,
                            showTraffic = ui.showTraffic,
                            modifier = Modifier.fillMaxWidth()
                        )
                    }
                }

                // 4e. Bottom Action Deck (5 equal-weight buttons + GO LIVE)
                BottomBar(
                    ui = ui,
                    selectedTool = selectedTool,
                    onSelectTool = { t ->
                        val next = if (selectedTool == t) null else t
                        selectedTool = next
                        if (next != null) {
                            showManualDeck = false
                            showEncoderDrawer = false
                        }
                    },
                    onToggleLive = { toggleLive() }
                )
            }
        }
    }
}

@Composable
private fun FrameGuidesOverlay(modifier: Modifier) = Canvas(modifier) {
    val c = Color.White.copy(alpha = 0.40f)
    for (i in 1..2) {
        drawLine(c, Offset(size.width * i / 3f, 0f), Offset(size.width * i / 3f, size.height), strokeWidth = 1f)
        drawLine(c, Offset(0f, size.height * i / 3f), Offset(size.width, size.height * i / 3f), strokeWidth = 1f)
    }
    drawRect(
        color = c,
        topLeft = Offset(size.width * 0.05f, size.height * 0.05f),
        size = Size(size.width * 0.9f, size.height * 0.9f),
        style = Stroke(width = 1f)
    )
}

@Composable
private fun TopHudBar(
    ui: HudUiState,
    fpsLabel: String,
    tally: TallyVisual,
    traffic: TrafficState,
    txStats: SenderStats,
    rxStats: ReceiverStats,
    battery: Int,
    onHideHud: () -> Unit,
    onSwitchMode: () -> Unit
) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Brush.verticalGradient(listOf(Color.Black.copy(alpha = 0.82f), Color.Transparent)))
            .padding(horizontal = 8.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(5.dp)
    ) {
        Row(
            Modifier
                .weight(1f)
                .horizontalScroll(rememberScrollState()),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(5.dp)
        ) {
            val (statusLabel, statusBg) = when {
                ui.senderActive -> "LIVE · TX" to Color(0xFF2D5F9A)
                ui.receiverActive -> "LIVE · RX" to Color(0xFF1F6E3E)
                else -> "OFFLINE" to Bm.dim
            }
            Text(
                statusLabel,
                style = Bm.mono(10.sp, FontWeight.Bold),
                color = Color.White,
                maxLines = 1,
                softWrap = false,
                modifier = Modifier
                    .background(statusBg, RoundedCornerShape(4.dp))
                    .padding(horizontal = 7.dp, vertical = 3.dp)
            )
            TallyBadge(tally)
            if (ui.showTimer) {
                Chip("⏱ ${traffic.timerText}", if (traffic.live) Bm.accent else Bm.dim)
            }
            if (ui.showTraffic) {
                val arrow = if (ui.mode == BroadcastMode.SENDER) "↑" else "↓"
                Chip("$arrow %.1f Mb/s".format(traffic.instantMbps), if (traffic.live) Bm.good else Bm.dim)
            }
            if (ui.mode == BroadcastMode.SENDER) {
                val fmtLabel = if (ui.codec == VideoCodecOption.DCT) {
                    "${ui.resolution.label} · $fpsLabel · DCT Q${ui.quality}"
                } else {
                    "${ui.resolution.label} · $fpsLabel · ${ui.codec.shortLabel} ${ui.bitrateMbps.toInt()}M"
                }
                Text(fmtLabel, style = Bm.mono(10.sp), color = Bm.text, maxLines = 1, softWrap = false)
                if (ui.senderActive) {
                    Chip("%.1f fps".format(txStats.fps), if (txStats.fps >= 55.0) Bm.good else Bm.warn)
                }
                if (ui.destIp.isNotBlank()) {
                    val pcLbl = if (ui.destName.isNotBlank()) "● ${ui.destName} :${ui.destPort}" else "● ${ui.destIp}:${ui.destPort}"
                    Chip(pcLbl, Bm.good)
                } else {
                    Chip("Пошук ПК…", Bm.warn)
                }
            } else {
                Text("UDP :${ui.listenPort} · AoIP ${ui.audioLatencyMs}ms", style = Bm.mono(10.sp), color = Bm.text, maxLines = 1, softWrap = false)
                if (ui.receiverActive) {
                    Chip("%.1f fps".format(rxStats.fps), if (rxStats.fps >= 55.0) Bm.good else Bm.warn)
                    Chip("E2E %.1f ms".format(rxStats.latP50), Bm.accent)
                }
            }
        }

        Text(
            if (battery >= 0) "$battery%" else "–",
            style = Bm.mono(10.sp),
            color = if (battery in 0..14) Bm.live else Bm.text,
            maxLines = 1,
            softWrap = false
        )
        Text(
            "HUD",
            style = Bm.mono(10.sp),
            color = Color.White,
            maxLines = 1,
            softWrap = false,
            modifier = Modifier
                .clickableNoRipple(onHideHud)
                .padding(horizontal = 3.dp)
        )
        Text(
            if (ui.mode == BroadcastMode.SENDER) "RX" else "TX",
            style = Bm.mono(10.sp, FontWeight.Bold),
            color = Bm.accent,
            maxLines = 1,
            softWrap = false,
            modifier = Modifier
                .background(Bm.panelHi, RoundedCornerShape(4.dp))
                .clickableNoRipple(onSwitchMode)
                .padding(horizontal = 6.dp, vertical = 3.dp)
        )
    }
}

@Composable
private fun TrafficAndTimerCard(
    traffic: TrafficState,
    isSender: Boolean,
    showTimer: Boolean,
    showTraffic: Boolean,
    modifier: Modifier = Modifier
) {
    Row(
        modifier
            .background(Color.Black.copy(alpha = 0.70f), RoundedCornerShape(8.dp))
            .border(1.dp, Bm.panelHi, RoundedCornerShape(8.dp))
            .padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        if (showTimer) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                Box(Modifier.size(7.dp).background(if (traffic.live) Bm.live else Bm.dim, CircleShape))
                Column {
                    Text(
                        if (traffic.live) "ON AIR" else "STREAM TIME",
                        style = Bm.mono(8.sp),
                        color = Bm.dim,
                        maxLines = 1
                    )
                    Text(
                        traffic.timerText,
                        style = Bm.mono(12.sp, FontWeight.Bold),
                        color = if (traffic.live) Bm.accent else Bm.text,
                        maxLines = 1
                    )
                }
            }
        }
        if (showTraffic) {
            val arrow = if (isSender) "↑" else "↓"
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    "$arrow %.1f Mb/s (%.2f MB/s)".format(traffic.instantMbps, traffic.instantMBps),
                    style = Bm.mono(11.sp, FontWeight.Bold),
                    color = if (traffic.live) Bm.good else Bm.dim,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
                Text(
                    "PEAK %.1f Mb/s · TOTAL %s".format(traffic.peakMbps, traffic.totalDataText),
                    style = Bm.mono(9.sp),
                    color = Bm.dim,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
private fun TallyBorder(tally: TallyVisual) {
    val (width, color) = when (tally) {
        TallyVisual.OFF -> 2.dp to Color.White.copy(alpha = 0.20f)
        TallyVisual.PREVIEW -> 6.dp to Bm.good
        TallyVisual.PROGRAM -> 6.dp to Bm.live
    }
    Box(
        Modifier
            .fillMaxSize()
            .padding(2.dp)
            .border(width, color, RoundedCornerShape(14.dp))
    )
}

@Composable
private fun TallyBadge(tally: TallyVisual, modifier: Modifier = Modifier) {
    val (fg, bg, border) = when (tally) {
        TallyVisual.OFF -> Triple(Bm.text.copy(alpha = 0.75f), Bm.panel.copy(alpha = 0.85f), Bm.dim)
        TallyVisual.PREVIEW -> Triple(Color.Black, Bm.good, Bm.good)
        TallyVisual.PROGRAM -> Triple(Color.White, Bm.live, Bm.live)
    }
    Row(
        modifier
            .background(bg, RoundedCornerShape(4.dp))
            .border(1.dp, border, RoundedCornerShape(4.dp))
            .padding(horizontal = 7.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Box(Modifier.size(6.dp).background(if (tally == TallyVisual.OFF) Bm.dim else fg, CircleShape))
        Text(tally.badgeLabel, style = Bm.mono(10.sp, FontWeight.Bold), color = fg, maxLines = 1, softWrap = false)
    }
}
