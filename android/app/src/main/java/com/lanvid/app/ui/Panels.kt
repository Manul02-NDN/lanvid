package com.lanvid.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.app.LanPeer
import com.lanvid.app.ReceiverStats
import com.lanvid.app.SenderStats
import kotlin.math.ln
import kotlin.math.pow

enum class BroadcastMode(val label: String) {
    SENDER("TX"),
    RECEIVER("RX")
}

enum class CaptureSource(val title: String, val shortLabel: String) {
    BACK_CAM("BACK CAM", "BACK"),
    FRONT_CAM("FRONT CAM", "FRONT"),
    SCREEN("SCREEN", "SCRN")
}

enum class VideoCodecOption(val id: Int, val title: String, val shortLabel: String) {
    DCT(0, "LanVid DCT", "DCT"),
    H264(1, "H.264 HW", "H264"),
    H265(2, "H.265 HW", "H265")
}

enum class AudioSourceOption(val id: Int, val title: String) {
    OFF(0, "OFF"),
    SYSTEM(1, "SYSTEM"),
    MIC(2, "MIC")
}

enum class ResolutionPreset(val width: Int, val height: Int, val label: String) {
    HD_720(1280, 720, "720p"),
    FHD_1080(1920, 1080, "1080p"),
    UHD_4K(3840, 2160, "4K")
}

enum class HudTool(val title: String) {
    SOURCE("SOURCE"),
    CODEC("CODEC"),
    NETWORK("LANVID"),
    AUDIO("AUDIO"),
    MONITOR("MON")
}

data class HudUiState(
    val mode: BroadcastMode = BroadcastMode.SENDER,
    val source: CaptureSource = CaptureSource.BACK_CAM,
    val resolution: ResolutionPreset = ResolutionPreset.FHD_1080,
    val audioSource: AudioSourceOption = AudioSourceOption.OFF,
    val codec: VideoCodecOption = VideoCodecOption.H265,
    val bitrateMbps: Double = 25.0,
    val quality: Int = 80,
    val fecGroup: Int = 6,
    val adaptive: Boolean = true,
    val destIp: String = "",
    val destPort: Int = 5006,
    val destName: String = "",
    val listenPort: Int = 5006,
    val volume: Float = 1.0f,
    val audioLatencyMs: Int = 20,
    val frameGuides: Boolean = true,
    val previewCropToFill: Boolean = true,
    val showTimer: Boolean = true,
    val showTraffic: Boolean = true,
    val senderActive: Boolean = false,
    val receiverActive: Boolean = false,
    val statusMessage: String? = null
) {
    val isLive: Boolean get() = if (mode == BroadcastMode.SENDER) senderActive else receiverActive

    fun toolSummary(tool: HudTool): String = when (tool) {
        HudTool.SOURCE -> if (mode == BroadcastMode.SENDER) source.shortLabel else "RX"
        HudTool.CODEC -> if (codec == VideoCodecOption.DCT) "Q$quality" else "${codec.shortLabel} ${bitrateMbps.toInt()}M"
        HudTool.NETWORK -> if (mode == BroadcastMode.SENDER) {
            if (destName.isNotBlank()) destName.take(8) else if (destIp.isNotBlank()) ":${destPort}" else "ПОШУК"
        } else ":${listenPort}"
        HudTool.AUDIO -> if (mode == BroadcastMode.SENDER) audioSource.title else "${audioLatencyMs}ms"
        HudTool.MONITOR -> if (frameGuides) "GRID" else "OFF"
    }
}

@Composable
fun BottomBar(
    ui: HudUiState,
    selectedTool: HudTool?,
    onSelectTool: (HudTool) -> Unit,
    onToggleLive: () -> Unit,
    modifier: Modifier = Modifier
) {
    Row(
        modifier
            .fillMaxWidth()
            .background(Brush.verticalGradient(listOf(Color.Transparent, Color.Black.copy(alpha = 0.88f))))
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Row(
            Modifier.weight(1f),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            HudTool.entries.forEach { t ->
                val sel = selectedTool == t
                Column(
                    Modifier
                        .weight(1f)
                        .background(
                            if (sel) Bm.accent else Bm.panelHi.copy(alpha = 0.90f),
                            RoundedCornerShape(6.dp)
                        )
                        .clickableNoRipple { onSelectTool(t) }
                        .padding(horizontal = 3.dp, vertical = 6.dp),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Text(
                        t.title,
                        style = Bm.mono(8.sp),
                        color = if (sel) Color.Black else Bm.dim,
                        maxLines = 1,
                        softWrap = false,
                        overflow = TextOverflow.Ellipsis
                    )
                    Text(
                        ui.toolSummary(t),
                        style = Bm.mono(10.sp, FontWeight.SemiBold),
                        color = if (sel) Color.Black else Bm.text,
                        maxLines = 1,
                        softWrap = false,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }

        Spacer(Modifier.width(8.dp))

        val liveBg = when {
            ui.isLive -> Bm.live
            ui.mode == BroadcastMode.RECEIVER -> Color(0xFF1F6E3E)
            else -> Color(0xFF404040)
        }
        val liveLabel = when {
            ui.isLive -> "STOP"
            ui.mode == BroadcastMode.RECEIVER -> "LISTEN"
            else -> "GO LIVE"
        }
        Box(
            Modifier
                .size(width = 82.dp, height = 42.dp)
                .background(liveBg, RoundedCornerShape(8.dp))
                .clickableNoRipple(onToggleLive),
            contentAlignment = Alignment.Center
        ) {
            Text(
                liveLabel,
                style = Bm.mono(12.sp, FontWeight.Bold),
                color = Color.White,
                maxLines = 1,
                softWrap = false
            )
        }
    }
}

@Composable
fun ValueSlider(
    value: Double,
    range: ClosedFloatingPointRange<Double>,
    log: Boolean = false,
    onChange: (Double) -> Unit
) {
    val lo = range.start
    val hi = range.endInclusive.coerceAtLeast(lo + 1e-9)
    fun toT(v: Double) = v.coerceIn(lo, hi).let {
        if (log && lo > 0) ln(it / lo) / ln(hi / lo) else (it - lo) / (hi - lo)
    }
    fun fromT(t: Double) = if (log && lo > 0) lo * (hi / lo).pow(t) else lo + t * (hi - lo)
    Slider(
        value = toT(value).toFloat(),
        onValueChange = { onChange(fromT(it.toDouble())) },
        colors = SliderDefaults.colors(
            thumbColor = Bm.accent,
            activeTrackColor = Bm.accent,
            inactiveTrackColor = Bm.panelHi
        )
    )
}

@Composable
private fun Row2(title: String, value: String, trailing: @Composable RowScope.() -> Unit = {}) =
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = Bm.mono(12.sp), color = Bm.dim)
        Spacer(Modifier.width(8.dp))
        Text(value, style = Bm.mono(12.sp), color = Bm.accent)
        Spacer(Modifier.weight(1f))
        trailing()
    }

@Composable
private fun SectionHeader(title: String) =
    Text(title, style = Bm.mono(10.sp), color = Bm.accent, modifier = Modifier.padding(top = 4.dp))

@Composable
private fun <T> Segments(
    options: List<T>,
    selected: T,
    label: (T) -> String,
    onSelect: (T) -> Unit
) = Row(
    Modifier.horizontalScroll(rememberScrollState()),
    horizontalArrangement = Arrangement.spacedBy(5.dp)
) {
    options.forEach { o ->
        FilterChip(
            selected = o == selected,
            onClick = { onSelect(o) },
            label = { Text(label(o), style = Bm.mono(11.sp)) },
            colors = FilterChipDefaults.filterChipColors(
                containerColor = Bm.panelHi,
                labelColor = Bm.text,
                selectedContainerColor = Bm.accent,
                selectedLabelColor = Color.Black
            )
        )
    }
}

@Composable
private fun <T> PresetRow(
    options: List<T>,
    current: T,
    label: (T) -> String,
    onSelect: (T) -> Unit
) = Row(
    Modifier.horizontalScroll(rememberScrollState()),
    horizontalArrangement = Arrangement.spacedBy(6.dp)
) {
    options.forEach { v ->
        val sel = v == current
        OutlinedButton(
            onClick = { onSelect(v) },
            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp)
        ) {
            Text(label(v), style = Bm.mono(10.sp), color = if (sel) Bm.accent else Bm.text)
        }
    }
}

@Composable
fun SwitchRow(
    label: String,
    checked: Boolean,
    enabled: Boolean = true,
    onChange: (Boolean) -> Unit
) = Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
    Text(
        label,
        style = Bm.mono(12.sp),
        color = if (enabled) Bm.text else Bm.dim,
        modifier = Modifier.weight(1f)
    )
    Switch(
        checked = checked,
        onCheckedChange = onChange,
        enabled = enabled,
        colors = SwitchDefaults.colors(
            checkedThumbColor = Color.Black,
            checkedTrackColor = Bm.accent
        )
    )
}

@Composable
private fun TextSetting(
    label: String,
    value: String,
    numeric: Boolean = false,
    onCommit: (String) -> Unit
) {
    var text by remember(value) { mutableStateOf(value) }
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(label, style = Bm.mono(10.sp), color = Bm.dim, modifier = Modifier.width(106.dp))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            singleLine = true,
            textStyle = Bm.mono(12.sp),
            modifier = Modifier.weight(1f),
            keyboardOptions = KeyboardOptions(
                keyboardType = if (numeric) KeyboardType.Number else KeyboardType.Text,
                imeAction = ImeAction.Done
            ),
            keyboardActions = KeyboardActions(onDone = { onCommit(text.trim()) })
        )
        OutlinedButton(
            onClick = { onCommit(text.trim()) },
            enabled = text.trim() != value,
            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp)
        ) {
            Text("Set", style = Bm.mono(10.sp), color = if (text.trim() != value) Bm.accent else Bm.dim)
        }
    }
}

@Composable
fun ToolPanel(
    tool: HudTool,
    ui: HudUiState,
    appVersion: String,
    simdInfo: String,
    txStats: SenderStats,
    rxStats: ReceiverStats,
    lanPeers: List<LanPeer>,
    scanning: Boolean,
    onScanLan: () -> Unit,
    onSelectPeerAndGoLive: ((LanPeer) -> Unit)? = null,
    onUpdate: (HudUiState) -> Unit,
    onSwitchMode: (BroadcastMode) -> Unit,
    onVolumeChange: (Float) -> Unit,
    onAudioLatencyChange: (Int) -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier
            .widthIn(max = 560.dp)
            .heightIn(max = 360.dp)
            .background(Bm.panel.copy(alpha = 0.95f), RoundedCornerShape(10.dp))
            .padding(12.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(7.dp)
    ) {
        when (tool) {
            HudTool.SOURCE -> {
                SectionHeader("MODE")
                Segments(BroadcastMode.entries, ui.mode, { if (it == BroadcastMode.SENDER) "SENDER (TX)" else "RECEIVER (RX)" }) {
                    onSwitchMode(it)
                }
                if (ui.mode == BroadcastMode.SENDER) {
                    SectionHeader("CAMERA / CAPTURE SOURCE")
                    Segments(CaptureSource.entries, ui.source, { it.title }) {
                        onUpdate(ui.copy(source = it))
                    }
                    SectionHeader("RESOLUTION")
                    Segments(ResolutionPreset.entries, ui.resolution, { "${it.label} (${it.width}×${it.height})" }) {
                        onUpdate(ui.copy(resolution = it))
                    }
                    SectionHeader("AUDIO INPUT")
                    Segments(AudioSourceOption.entries, ui.audioSource, { it.title }) {
                        onUpdate(ui.copy(audioSource = it))
                    }
                    Text(
                        "Front camera auto-rotates 270° + mirrors horizontally with strict YUV_420_888 stride safety.",
                        style = Bm.mono(10.sp),
                        color = Bm.dim
                    )
                } else {
                    Text(
                        "Receiver mode listens on UDP :${ui.listenPort} with ANativeWindow rendering and AAudio low-latency playback.",
                        style = Bm.mono(10.sp),
                        color = Bm.dim
                    )
                }
            }

            HudTool.CODEC -> {
                SectionHeader("LANVID ENCODER")
                Segments(VideoCodecOption.entries, ui.codec, { it.title }) {
                    onUpdate(ui.copy(codec = it))
                }
                if (ui.codec != VideoCodecOption.DCT) {
                    Row2("${ui.codec.title} BITRATE", "${ui.bitrateMbps.toInt()} Mb/s")
                    ValueSlider(ui.bitrateMbps, 1.0..150.0, log = true) {
                        onUpdate(ui.copy(bitrateMbps = Math.round(it).toDouble().coerceIn(1.0, 150.0)))
                    }
                    PresetRow(listOf(5, 15, 25, 50, 100), ui.bitrateMbps.toInt(), { "$it Mb" }) {
                        onUpdate(ui.copy(bitrateMbps = it.toDouble()))
                    }
                }
                Row2(
                    if (ui.codec == VideoCodecOption.DCT) "DCT QUALITY" else "AIMD QUALITY TARGET",
                    "Q${ui.quality}"
                )
                ValueSlider(ui.quality.toDouble(), 30.0..95.0) {
                    onUpdate(ui.copy(quality = Math.round(it).toInt().coerceIn(30, 95)))
                }
                PresetRow(listOf(60, 75, 80, 90, 95), ui.quality, { "Q$it" }) {
                    onUpdate(ui.copy(quality = it))
                }
                Row2("XOR FEC GROUP", if (ui.fecGroup == 0) "OFF" else "${ui.fecGroup} pkts")
                ValueSlider(ui.fecGroup.toDouble(), 0.0..16.0) {
                    onUpdate(ui.copy(fecGroup = Math.round(it).toInt().coerceIn(0, 16)))
                }
                PresetRow(listOf(0, 4, 6, 8), ui.fecGroup, { if (it == 0) "OFF" else "FEC $it" }) {
                    onUpdate(ui.copy(fecGroup = it))
                }
                SwitchRow("Adaptive quality / bitrate (AIMD)", ui.adaptive) {
                    onUpdate(ui.copy(adaptive = it))
                }
            }

            HudTool.NETWORK -> {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("LANVID CORE", style = Bm.mono(12.sp), color = Bm.dim)
                    Text("v$appVersion", style = Bm.mono(12.sp), color = Bm.accent)
                    Spacer(Modifier.weight(1f))
                    Chip(simdInfo.uppercase(), Bm.good)
                    Chip(if (ui.isLive) "LIVE" else "IDLE", if (ui.isLive) Bm.good else Bm.dim)
                }

                if (ui.mode == BroadcastMode.SENDER) {
                    Row(
                        Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Text("ПК ПРИЙМАЧ", style = Bm.mono(10.sp), color = Bm.dim)
                        Text(
                            if (ui.destIp.isNotBlank()) {
                                val nm = if (ui.destName.isNotBlank()) "${ui.destName} · " else ""
                                "$nm${ui.destIp}:${ui.destPort}"
                            } else "Пошук ПК у Wi-Fi...",
                            style = Bm.mono(11.sp),
                            color = if (ui.destIp.isNotBlank()) Bm.good else Bm.warn,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f)
                        )
                        OutlinedButton(
                            onClick = onScanLan,
                            enabled = !scanning,
                            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp)
                        ) {
                            Text(if (scanning) "Пошук…" else "Оновити", style = Bm.mono(10.sp), color = Bm.accent)
                        }
                    }

                    if (lanPeers.isNotEmpty()) {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            lanPeers.forEach { p ->
                                val sel = ui.destIp == p.ip && ui.destPort == p.port
                                Row(
                                    Modifier
                                        .fillMaxWidth()
                                        .background(
                                            if (sel) Bm.accent.copy(alpha = 0.22f) else Bm.panelHi.copy(alpha = 0.85f),
                                            RoundedCornerShape(8.dp)
                                        )
                                        .clickableNoRipple {
                                            onUpdate(ui.copy(destIp = p.ip, destPort = p.port, destName = p.name))
                                        }
                                        .padding(horizontal = 10.dp, vertical = 8.dp),
                                    verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                                ) {
                                    Chip(p.roleBadge, if (p.role == "video") Bm.good else Bm.accent)
                                    Column(Modifier.weight(1f)) {
                                        Text(p.name, style = Bm.mono(11.sp, FontWeight.Bold), color = Color.White, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                        Text("${p.ip}:${p.port} · ${p.rttMs} ms", style = Bm.mono(9.sp), color = Bm.dim)
                                    }
                                    if (onSelectPeerAndGoLive != null && !ui.senderActive && p.role != "audio") {
                                        Button(
                                            onClick = { onSelectPeerAndGoLive(p) },
                                            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                                            colors = ButtonDefaults.buttonColors(containerColor = Bm.good)
                                        ) {
                                            Text("GO LIVE", style = Bm.mono(10.sp, FontWeight.Bold), color = Color.Black)
                                        }
                                    } else if (sel) {
                                        Text("ОБРАНО", style = Bm.mono(10.sp, FontWeight.Bold), color = Bm.good)
                                    }
                                }
                            }
                        }
                    } else {
                        Text(
                            "Автопошук активний (mDNS + LAN Broadcast :5023 / :5099). Запустіть LanVid Video або LanVid Studio на ПК у тій же Wi-Fi мережі — ПК з'явиться тут автоматично.",
                            style = Bm.mono(10.sp),
                            color = Bm.dim
                        )
                    }

                    TextSetting("IP ПК (ВРУЧНУ)", ui.destIp) { onUpdate(ui.copy(destIp = it, destName = "")) }
                    TextSetting("UDP ПОРТ", ui.destPort.toString(), numeric = true) { v ->
                        v.toIntOrNull()?.let { p -> onUpdate(ui.copy(destPort = p.coerceIn(1024, 65535))) }
                    }
                    PresetRow(listOf(5006, 5000, 5001), ui.destPort, { ":$it" }) {
                        onUpdate(ui.copy(destPort = it))
                    }
                } else {
                    TextSetting("LISTEN PORT", ui.listenPort.toString(), numeric = true) { v ->
                        v.toIntOrNull()?.let { p -> onUpdate(ui.copy(listenPort = p.coerceIn(1024, 65535))) }
                    }
                    PresetRow(listOf(5000, 5001, 5002), ui.listenPort, { ":$it" }) {
                        onUpdate(ui.copy(listenPort = it))
                    }
                }
            }

            HudTool.AUDIO -> {
                SectionHeader("AUDIO & 64-CH AoIP BUS")
                Row2("AAUDIO VOLUME", "${(ui.volume * 100).toInt()}%") {
                    TextButton(onClick = { onVolumeChange(1.0f) }) {
                        Text("100%", style = Bm.mono(10.sp), color = Bm.accent)
                    }
                }
                ValueSlider(ui.volume.toDouble(), 0.0..1.5) {
                    onVolumeChange(it.toFloat().coerceIn(0f, 1.5f))
                }
                Row2("AoIP BUS LATENCY", "${ui.audioLatencyMs} ms")
                ValueSlider(ui.audioLatencyMs.toDouble(), 2.0..2000.0, log = true) {
                    onAudioLatencyChange(Math.round(it).toInt().coerceIn(2, 2000))
                }
                PresetRow(listOf(5, 10, 20, 50, 200, 1000), ui.audioLatencyMs, { "$it ms" }) {
                    onAudioLatencyChange(it)
                }
                Text(
                    "Adaptive reliability ladder: 2–10 ms (low-latency FEC), 10–100 ms (FEC+NACK), 100–2000 ms (interleaved FEC + multi-NACK).",
                    style = Bm.mono(10.sp),
                    color = Bm.dim
                )
            }

            HudTool.MONITOR -> {
                Text("OPERATOR MONITORING — NEVER SENT OVER LAN", style = Bm.mono(10.sp), color = Bm.accent)
                SwitchRow("Frame guides (rule of thirds + safe area)", ui.frameGuides) {
                    onUpdate(ui.copy(frameGuides = it))
                }
                SwitchRow("Fill viewfinder (Crop vs Fit)", ui.previewCropToFill) {
                    onUpdate(ui.copy(previewCropToFill = it))
                }
                SwitchRow("Stream uptime timer", ui.showTimer) {
                    onUpdate(ui.copy(showTimer = it))
                }
                SwitchRow("Live traffic overview (Mb/s & sparkline)", ui.showTraffic) {
                    onUpdate(ui.copy(showTraffic = it))
                }
                SectionHeader("LIVE TELEMETRY")
                if (ui.mode == BroadcastMode.SENDER) {
                    Row(
                        Modifier.horizontalScroll(rememberScrollState()),
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Chip("${txStats.width}×${txStats.height}", Bm.text)
                        Chip("%.1f fps".format(txStats.fps), Bm.good)
                        Chip("ENC %.1f ms".format(txStats.encMs), Bm.text)
                        Chip("Q${txStats.quality}", Bm.accent)
                        Chip("AUD %.0f kb/s".format(txStats.audioKbps), Bm.text)
                    }
                } else {
                    Row(
                        Modifier.horizontalScroll(rememberScrollState()),
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Chip("${rxStats.width}×${rxStats.height}", Bm.text)
                        Chip("%.1f fps".format(rxStats.fps), Bm.good)
                        Chip("E2E %.1f/%.1f ms".format(rxStats.latP50, rxStats.latP99), Bm.accent)
                        Chip("FEC ${rxStats.fecRecovered}", Bm.good)
                        Chip("LOSS %.2f%%".format(rxStats.lossPct), if (rxStats.lossPct > 0.5) Bm.warn else Bm.text)
                        Chip("BUF %.1f ms".format(rxStats.audioBufMs), Bm.text)
                    }
                }
            }
        }
    }
}
