package com.lanvid.app

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {
    private lateinit var cameraStreamer: Camera2Streamer
    private val micStreamer = AudioInputStreamer()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        LanVidNative.nativeInit()
        cameraStreamer = Camera2Streamer(this)

        setContent {
            val darkColors = darkColorScheme(
                primary = Color(0xFF38BDF8),
                secondary = Color(0xFF22C55E),
                background = Color(0xFF0F131A),
                surface = Color(0xFF171D28),
                onBackground = Color(0xFFEFF4FA),
                onSurface = Color(0xFFEFF4FA)
            )
            MaterialTheme(colorScheme = darkColors) {
                LanVidAndroidApp(
                    cameraStreamer = cameraStreamer,
                    micStreamer = micStreamer
                )
            }
        }
    }

    override fun onDestroy() {
        cameraStreamer.stop()
        micStreamer.stop()
        LanVidNative.nativeStopSender()
        LanVidNative.nativeStopReceiver()
        LanVidNative.nativeSetReceiverSurface(null)
        super.onDestroy()
    }
}

@Composable
fun LanVidAndroidApp(
    cameraStreamer: Camera2Streamer,
    micStreamer: AudioInputStreamer
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val simdInfo = remember { LanVidNative.nativeGetSimdInfo() }

    // Auto-request runtime permissions on first launch
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { _ -> }

    LaunchedEffect(Unit) {
        val perms = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.POST_NOTIFICATIONS
        }
        permLauncher.launch(perms.toTypedArray())
    }

    var selectedTab by remember { mutableIntStateOf(0) } // 0=Sender, 1=Receiver
    var destIp by remember { mutableStateOf("192.168.1.100") }
    var destPort by remember { mutableStateOf("5000") }
    var listenPort by remember { mutableStateOf("5000") }
    var sourceKind by remember { mutableIntStateOf(0) } // 0=Camera2 YUV420, 1=Screen MediaProjection
    var frontCamera by remember { mutableStateOf(false) }
    var audioMode by remember { mutableIntStateOf(0) } // 0=Off, 1=Internal, 2=Mic
    var quality by remember { mutableFloatStateOf(78f) }
    var fecGroup by remember { mutableIntStateOf(6) }
    var adaptive by remember { mutableStateOf(true) }
    var volume by remember { mutableFloatStateOf(1.0f) }

    var senderActive by remember { mutableStateOf(false) }
    var receiverActive by remember { mutableStateOf(false) }
    var statusMessage by remember { mutableStateOf("Готово до роботи") }
    var lanPeers by remember { mutableStateOf<List<LanPeer>>(emptyList()) }
    var scanning by remember { mutableStateOf(false) }

    var txStats by remember { mutableStateOf(SenderStats()) }
    var rxStats by remember { mutableStateOf(ReceiverStats()) }

    val projectionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { res ->
        if (res.resultCode == Activity.RESULT_OK && res.data != null) {
            val dm: DisplayMetrics = context.resources.displayMetrics
            val capW = 1280
            val capH = (1280 * dm.heightPixels / dm.widthPixels.coerceAtLeast(1)) and 0x7FFE
            val ok = LanVidNative.nativeStartSender(
                destIp,
                destPort.toIntOrNull() ?: 5000,
                capW,
                capH.coerceIn(480, 1440),
                60,
                quality.toInt(),
                fecGroup,
                adaptive
            )
            if (ok) {
                val svcIntent = Intent(context, ScreenCaptureService::class.java).apply {
                    action = ScreenCaptureService.ACTION_START
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, res.resultCode)
                    putExtra(ScreenCaptureService.EXTRA_RESULT_DATA, res.data)
                    putExtra(ScreenCaptureService.EXTRA_WIDTH, capW)
                    putExtra(ScreenCaptureService.EXTRA_HEIGHT, capH.coerceIn(480, 1440))
                    putExtra(ScreenCaptureService.EXTRA_DPI, dm.densityDpi)
                    putExtra(ScreenCaptureService.EXTRA_AUDIO_MODE, audioMode)
                }
                context.startForegroundService(svcIntent)
                senderActive = true
                statusMessage = "Трансляція екрана -> $destIp:$destPort"
            }
        } else {
            statusMessage = "Користувач скасував доступ до екрана"
        }
    }

    // Periodic stats polling + initial LAN discovery
    LaunchedEffect(Unit) {
        scanning = true
        lanPeers = withContext(Dispatchers.IO) { LanVidNative.scanLanPeers(450) }
        scanning = false
        while (true) {
            delay(500)
            txStats = LanVidNative.senderStats()
            rxStats = LanVidNative.receiverStats()
        }
    }

    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Column(modifier = Modifier.fillMaxSize().padding(14.dp)) {
            // Header
            Row(
                modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        "LanVid Studio Android",
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        color = MaterialTheme.colorScheme.primary
                    )
                    Text(
                        "v2.1 | Core SIMD: $simdInfo | ${Build.MODEL}",
                        fontSize = 12.sp,
                        color = Color(0xFF94A3B8)
                    )
                }
            }

            // Tab Switcher
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { selectedTab = 0 },
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (selectedTab == 0) Color(0xFF2563EB) else Color(0xFF1E293B)
                    )
                ) { Text("Передача (Sender)") }

                Button(
                    onClick = { selectedTab = 1 },
                    modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (selectedTab == 1) Color(0xFF2563EB) else Color(0xFF1E293B)
                    )
                ) { Text("Прийом (Receiver)") }
            }

            Spacer(Modifier.height(10.dp))

            if (selectedTab == 0) {
                Column(
                    modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    // Source & Audio Card
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        shape = RoundedCornerShape(10.dp)
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("Джерело відео та звуку", fontWeight = FontWeight.SemiBold)
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                FilterChip(
                                    selected = sourceKind == 0,
                                    onClick = { sourceKind = 0 },
                                    label = { Text("Камера (YUV420)") }
                                )
                                FilterChip(
                                    selected = sourceKind == 1,
                                    onClick = { sourceKind = 1 },
                                    label = { Text("Екран (MediaProjection)") }
                                )
                            }
                            if (sourceKind == 0) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Checkbox(checked = frontCamera, onCheckedChange = { frontCamera = it })
                                    Text("Фронтальна камера")
                                }
                            }
                            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                FilterChip(
                                    selected = audioMode == 0,
                                    onClick = { audioMode = 0 },
                                    label = { Text("Без звуку") }
                                )
                                FilterChip(
                                    selected = audioMode == 1,
                                    onClick = { audioMode = 1 },
                                    label = { Text("Системний") }
                                )
                                FilterChip(
                                    selected = audioMode == 2,
                                    onClick = { audioMode = 2 },
                                    label = { Text("Мікрофон") }
                                )
                            }
                        }
                    }

                    // LAN Discovery & Target Card
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        shape = RoundedCornerShape(10.dp)
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                horizontalArrangement = Arrangement.SpaceBetween,
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Text("Отримувач у Wi-Fi / LAN", fontWeight = FontWeight.SemiBold)
                                TextButton(onClick = {
                                    scope.launch {
                                        scanning = true
                                        lanPeers = withContext(Dispatchers.IO) { LanVidNative.scanLanPeers(450) }
                                        scanning = false
                                    }
                                }) { Text(if (scanning) "Пошук..." else "Сканувати LAN") }
                            }

                            if (lanPeers.isEmpty()) {
                                Text(
                                    "Натисніть «Сканувати LAN» або введіть IP вручну",
                                    fontSize = 12.sp,
                                    color = Color(0xFF94A3B8)
                                )
                            } else {
                                lanPeers.forEach { peer ->
                                    Row(
                                        modifier = Modifier
                                            .fillMaxWidth()
                                            .background(Color(0xFF222C3C), RoundedCornerShape(6.dp))
                                            .clickable {
                                                destIp = peer.ip
                                                destPort = peer.port.toString()
                                            }
                                            .padding(8.dp),
                                        horizontalArrangement = Arrangement.SpaceBetween
                                    ) {
                                        Text("${peer.name} [${peer.os}]", fontSize = 13.sp)
                                        Text("${peer.ip}:${peer.port} (${peer.rttMs} ms)", fontSize = 12.sp, color = Color(0xFF38BDF8))
                                    }
                                }
                            }

                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                OutlinedTextField(
                                    value = destIp,
                                    onValueChange = { destIp = it },
                                    label = { Text("IP отримувача") },
                                    modifier = Modifier.weight(2f),
                                    singleLine = true
                                )
                                OutlinedTextField(
                                    value = destPort,
                                    onValueChange = { destPort = it },
                                    label = { Text("Порт") },
                                    modifier = Modifier.weight(1f),
                                    singleLine = true
                                )
                            }
                        }
                    }

                    // Quality / FEC / AIMD Card
                    Card(
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        shape = RoundedCornerShape(10.dp)
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text("Якість: Q=${quality.toInt()} | XOR FEC=$fecGroup | AIMD=${if (adaptive) "ON" else "OFF"}")
                            Slider(value = quality, onValueChange = { quality = it }, valueRange = 30f..95f)
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Checkbox(checked = adaptive, onCheckedChange = { adaptive = it })
                                Text("Wi-Fi Adaptive (AIMD) + XOR FEC")
                            }
                        }
                    }

                    // Start / Stop Sender Button
                    Button(
                        onClick = {
                            if (!senderActive) {
                                if (sourceKind == 1) {
                                    val mpm = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                                    projectionLauncher.launch(mpm.createScreenCaptureIntent())
                                } else {
                                    val ok = LanVidNative.nativeStartSender(
                                        destIp,
                                        destPort.toIntOrNull() ?: 5000,
                                        1280,
                                        720,
                                        60,
                                        quality.toInt(),
                                        fecGroup,
                                        adaptive
                                    )
                                    if (ok) {
                                        cameraStreamer.start(1280, 720, frontCamera) { err ->
                                            statusMessage = err
                                        }
                                        if (audioMode == 2) micStreamer.start(null)
                                        senderActive = true
                                        statusMessage = "Камера (YUV420) -> $destIp:$destPort"
                                    }
                                }
                            } else {
                                cameraStreamer.stop()
                                micStreamer.stop()
                                if (ScreenCaptureService.isRunning) {
                                    context.startService(
                                        Intent(context, ScreenCaptureService::class.java).apply {
                                            action = ScreenCaptureService.ACTION_STOP
                                        }
                                    )
                                }
                                LanVidNative.nativeStopSender()
                                senderActive = false
                                statusMessage = "Передачу зупинено"
                            }
                        },
                        modifier = Modifier.fillMaxWidth().height(50.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = if (senderActive) Color(0xFFDC2626) else Color(0xFF16A34A)
                        )
                    ) {
                        Text(if (senderActive) "СТОП ПЕРЕДАЧІ" else "СТАРТ ПЕРЕДАЧІ", fontWeight = FontWeight.Bold)
                    }

                    // Live Sender Telemetry
                    Text(
                        "TX: ${txStats.width}x${txStats.height} | %.1f FPS | %.1f Mbps | enc %.2f ms | Q=%d | Aud %.0f kbps"
                            .format(txStats.fps, txStats.mbps, txStats.encMs, txStats.quality, txStats.audioKbps),
                        fontSize = 12.sp,
                        color = Color(0xFF38BDF8)
                    )
                    Text(statusMessage, fontSize = 12.sp, color = Color(0xFF94A3B8))
                }
            } else {
                // RECEIVER TAB
                Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        OutlinedTextField(
                            value = listenPort,
                            onValueChange = { listenPort = it },
                            label = { Text("UDP Порт") },
                            modifier = Modifier.width(120.dp),
                            singleLine = true
                        )
                        Button(
                            onClick = {
                                if (!receiverActive) {
                                    val p = listenPort.toIntOrNull() ?: 5000
                                    if (LanVidNative.nativeStartReceiver(p, 12.0, Build.MODEL)) {
                                        LanVidNative.nativeSetVolume(volume)
                                        receiverActive = true
                                        statusMessage = "Прийом на UDP :$p (AAudio + LAN автопошук)"
                                    }
                                } else {
                                    LanVidNative.nativeStopReceiver()
                                    receiverActive = false
                                    statusMessage = "Прийом зупинено"
                                }
                            },
                            modifier = Modifier.weight(1f).height(52.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = if (receiverActive) Color(0xFFDC2626) else Color(0xFF16A34A)
                            )
                        ) {
                            Text(if (receiverActive) "СТОП ПРИЙОМУ" else "СТАРТ ПРИЙОМУ", fontWeight = FontWeight.Bold)
                        }
                    }

                    // Native ANativeWindow Video Surface
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .weight(1f)
                            .background(Color.Black, RoundedCornerShape(10.dp))
                    ) {
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
                    }

                    // Volume + Live Telemetry
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Гучність ${(volume * 100).toInt()}%", fontSize = 13.sp, modifier = Modifier.width(105.dp))
                        Slider(
                            value = volume,
                            onValueChange = {
                                volume = it
                                LanVidNative.nativeSetVolume(it)
                            },
                            valueRange = 0f..1.5f,
                            modifier = Modifier.weight(1f)
                        )
                    }

                    Text(
                        "RX: ${rxStats.width}x${rxStats.height} | %.1f FPS | E2E %.2f / %.2f ms | %.1f Mbps | FEC:%d | Loss %.2f%% | AAudio %.0f kbps (%.1f ms)"
                            .format(
                                rxStats.fps, rxStats.latP50, rxStats.latP99, rxStats.mbps,
                                rxStats.fecRecovered, rxStats.lossPct, rxStats.audioKbps, rxStats.audioBufMs
                            ),
                        fontSize = 12.sp,
                        color = Color(0xFF22C55E)
                    )
                }
            }
        }
    }
}
