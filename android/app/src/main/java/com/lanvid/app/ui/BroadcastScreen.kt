package com.lanvid.app.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.lanvid.app.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.max
import kotlin.math.min

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
        context.display?.rotation ?: Surface.ROTATION_0
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
    val scope = rememberCoroutineScope()
    val appVersion = remember { LanVidNative.nativeGetVersion() }
    val simdInfo = remember { LanVidNative.nativeGetSimdInfo() }

    var ui by remember { mutableStateOf(HudUiState()) }
    var selectedTool by remember { mutableStateOf<HudTool?>(null) }
    var showManualDeck by remember { mutableStateOf(false) }
    var showEncoderDrawer by remember { mutableStateOf(false) }
    var hudHidden by remember { mutableStateOf(false) }

    var lanPeers by remember { mutableStateOf<List<LanPeer>>(emptyList()) }
    var scanning by remember { mutableStateOf(false) }
    var txStats by remember { mutableStateOf(SenderStats()) }
    var rxStats by remember { mutableStateOf(ReceiverStats()) }
    var traffic by remember { mutableStateOf(TrafficState()) }
    var battery by remember { mutableIntStateOf(-1) }
    var proState by remember { mutableStateOf(ProCameraState()) }

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

    // Studio Tally & Duplex Intercom connection + 10 Hz status poll
    LaunchedEffect(ui.mode, ui.senderActive, ui.destIp) {
        if (ui.mode == BroadcastMode.SENDER && ui.senderActive && ui.destIp.isNotBlank()) {
            LanVidNative.nativeTallyConnect(ui.destIp, 5022, 1, proState.cameraLabel)
            LanVidNative.nativeIntercomSetState(proState.pttHeld || proState.latchTalk, proState.ifbMuted, proState.ifbGain, 0f)
            while (true) {
                delay(100)
                val raw = LanVidNative.nativeTallyPollJson(battery.coerceAtLeast(0), 34)
                if (raw.isNotEmpty()) {
                    runCatching {
                        val obj = JSONObject(raw)
                        val tVal = obj.optInt("tally", 0)
                        val tState = when (tVal) {
                            2 -> TallyStatus.PROGRAM
                            1 -> TallyStatus.PREVIEW
                            else -> TallyStatus.STANDBY
                        }
                        proState = proState.copy(
                            tally = tState,
                            remoteRec = obj.optBoolean("remoteRec", false),
                            directorSpeaking = obj.optBoolean("directorSpeaking", false),
                            duckingGain = obj.optDouble("ducking", 1.0).toFloat(),
                            cameraLabel = obj.optString("label", proState.cameraLabel).ifBlank { proState.cameraLabel }
                        )
                    }
                }
            }
        } else {
            LanVidNative.nativeTallyDisconnect()
            proState = proState.copy(tally = TallyStatus.STANDBY, directorSpeaking = false, duckingGain = 1.0f)
        }
    }

    // Restart live Camera2 viewfinder whenever lens, resolution, surface, orientation, or mode changes
    LaunchedEffect(
        hasCameraPermission,
        ui.mode,
        ui.source,
        ui.resolution,
        displayRotation,
        cameraPreviewSurface
    ) {
        val surface = cameraPreviewSurface
        if (ui.mode == BroadcastMode.SENDER &&
            ui.source != CaptureSource.SCREEN &&
            hasCameraPermission &&
            surface != null &&
            surface.isValid
        ) {
            cameraStreamer.start(
                width = ui.resolution.width,
                height = ui.resolution.height,
                frontFacing = ui.source == CaptureSource.FRONT_CAM,
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
            val capH = (capW * dm.heightPixels / dm.widthPixels.coerceAtLeast(1)) and 0x7FFE
            val ok = LanVidNative.nativeStartSender(
                ui.destIp,
                ui.destPort,
                capW,
                capH.coerceIn(480, 1440),
                60,
                ui.quality,
                ui.fecGroup,
                ui.adaptive,
                ui.codec.id,
                (ui.bitrateMbps * 1000).toInt()
            )
            if (ok) {
                val svcIntent = Intent(context, ScreenCaptureService::class.java).apply {
                    action = ScreenCaptureService.ACTION_START
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, res.resultCode)
                    putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, res.data)
                    putExtra(ScreenCaptureService.EXTRA_WIDTH, capW)
                    putExtra(ScreenCaptureService.EXTRA_HEIGHT, capH.coerceIn(480, 1440))
                    putExtra(ScreenCaptureService.EXTRA_DPI, dm.densityDpi)
                    putExtra(ScreenCaptureService.EXTRA_AUDIO_MODE, ui.audioSource.id)
                }
                context.startForegroundService(svcIntent)
                ui = ui.copy(
                    senderActive = true,
                    statusMessage = "Screen cast -> ${ui.destIp}:${ui.destPort}"
                )
            }
        } else {
            ui = ui.copy(statusMessage = "Screen capture permission denied")
        }
    }

    // Battery monitor
    LaunchedEffect(Unit) {
        while (true) {
            battery = context.getSystemService(BatteryManager::class.java)
                ?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) ?: -1
            delay(10_000)
        }
    }

    // Initial LAN discovery + 500 ms telemetry & traffic sparkline loop
    LaunchedEffect(Unit) {
        scanning = true
        lanPeers = withContext(Dispatchers.IO) { LanVidNative.scanLanPeers(450) }
        scanning = false
        var liveStartMs = 0L
        var wasLive = false
        while (true) {
            delay(500)
            txStats = LanVidNative.senderStats()
            rxStats = LanVidNative.receiverStats()
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
        micStreamer.stop()
        if (ScreenCaptureService.isRunning) {
            context.startService(
                Intent(context, ScreenCaptureService::class.java).apply {
                    action = ScreenCaptureService.ACTION_STOP
                }
            )
        }
        LanVidNative.nativeStopSender()
        LanVidNative.nativeStopReceiver()
        LanVidNative.nativeTallyDisconnect()
        ui = ui.copy(senderActive = false, receiverActive = false)
    }

    fun toggleLive() {
        if (ui.mode == BroadcastMode.SENDER) {
            if (!ui.senderActive) {
                if (ui.source == CaptureSource.SCREEN) {
                    val mpm = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                    projectionLauncher.launch(mpm.createScreenCaptureIntent())
                } else {
                    // Pass true display-oriented rotated dimensions (e.g. 720x1280 in portrait, 1280x720 in landscape)
                    val encW = cameraStreamer.outputWidth.takeIf { it > 0 } ?: ui.resolution.width
                    val encH = cameraStreamer.outputHeight.takeIf { it > 0 } ?: ui.resolution.height
                    val ok = LanVidNative.nativeStartSender(
                        ui.destIp,
                        ui.destPort,
                        encW,
                        encH,
                        60,
                        ui.quality,
                        ui.fecGroup,
                        ui.adaptive,
                        ui.codec.id,
                        (ui.bitrateMbps * 1000).toInt()
                    )
                    if (ok) {
                        cameraStreamer.setStreamingEnabled(true)
                        if (ui.audioSource == AudioSourceOption.MIC) micStreamer.start(null)
                        ui = ui.copy(
                            senderActive = true,
                            statusMessage = "${ui.source.title} ${encW}×${encH} (${ui.codec.shortLabel}) -> ${ui.destIp}:${ui.destPort}"
                        )
                    } else {
                        ui = ui.copy(statusMessage = "Failed to start LanVid sender")
                    }
                }
            } else {
                stopAll()
                ui = ui.copy(statusMessage = "Stream stopped")
            }
        } else {
            if (!ui.receiverActive) {
                if (LanVidNative.nativeStartReceiver(ui.listenPort, ui.audioLatencyMs.toDouble(), Build.MODEL)) {
                    LanVidNative.nativeSetVolume(ui.volume)
                    LanVidNative.nativeSetAudioLatencyMs(ui.audioLatencyMs)
                    ui = ui.copy(
                        receiverActive = true,
                        statusMessage = "Listening on UDP :${ui.listenPort} (AAudio + Discovery)"
                    )
                } else {
                    ui = ui.copy(statusMessage = "Failed to bind UDP :${ui.listenPort}")
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
                                cameraStreamer.stop()
                                cameraSurfaceTexture = null
                                cameraPreviewSurface?.release()
                                cameraPreviewSurface = null
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
                                val prevLens = proState.lens
                                proState = next
                                if (next.lens != prevLens) {
                                    val nextSource = if (next.lens == "Front") CaptureSource.FRONT_CAM else CaptureSource.BACK_CAM
                                    if (ui.source != nextSource) ui = ui.copy(source = nextSource)
                                }
                            },
                            modifier = Modifier.align(Alignment.CenterStart)
                        )
                    }

                    if (ui.mode == BroadcastMode.SENDER && showEncoderDrawer) {
                        ProEncoderDrawer(
                            state = proState,
                            onStateChange = { next ->
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
                                ui = ui.copy(
                                    resolution = nextRes,
                                    codec = nextCodec,
                                    bitrateMbps = next.bitrateMbps.toDouble().coerceIn(1.0, 50.0)
                                )
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
                            onScanLan = {
                                if (!scanning) {
                                    scanning = true
                                    scope.launch {
                                        lanPeers = withContext(Dispatchers.IO) { LanVidNative.scanLanPeers(450) }
                                        scanning = false
                                    }
                                }
                            },
                            onUpdate = { ui = it },
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
                                LanVidNative.nativeIntercomSetState(talk, listenMuted, rxGain, 0f)
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
                    "${ui.resolution.label} · 60 · DCT Q${ui.quality}"
                } else {
                    "${ui.resolution.label} · 60 · ${ui.codec.shortLabel} ${ui.bitrateMbps.toInt()}M"
                }
                Text(fmtLabel, style = Bm.mono(10.sp), color = Bm.text, maxLines = 1, softWrap = false)
                if (ui.senderActive) {
                    Chip("%.1f fps".format(txStats.fps), if (txStats.fps >= 55.0) Bm.good else Bm.warn)
                }
                Chip("LANVID ${ui.destIp}:${ui.destPort}", Bm.accent)
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
