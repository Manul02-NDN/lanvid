package com.lanvid.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
    BACK_CAM("Основна", "BACK"),
    FRONT_CAM("Фронтальна", "FRONT"),
    SCREEN("Екран", "SCRN")
}

enum class VideoCodecOption(val id: Int, val title: String, val shortLabel: String) {
    DCT(0, "LanVid DCT", "DCT"),
    H264(1, "H.264 HW", "H264"),
    H265(2, "H.265 HW", "H265")
}

enum class AudioSourceOption(val id: Int, val title: String) {
    OFF(0, "Вимк."),
    SYSTEM(1, "Системний"),
    MIC(2, "Мікрофон")
}

enum class ResolutionPreset(val width: Int, val height: Int, val label: String) {
    HD_720(1280, 720, "720p"),
    FHD_1080(1920, 1080, "1080p"),
    UHD_4K(3840, 2160, "4K")
}

enum class HudTool(val title: String) {
    SOURCE("Джерело"),
    CODEC("Якість"),
    NETWORK("Мережа"),
    AUDIO("Аудіо"),
    MONITOR("Монітор")
}

data class HudUiState(
    val mode: BroadcastMode = BroadcastMode.SENDER,
    val source: CaptureSource = CaptureSource.BACK_CAM,
    val resolution: ResolutionPreset = ResolutionPreset.FHD_1080,
    val audioSource: AudioSourceOption = AudioSourceOption.OFF,
    val codec: VideoCodecOption = VideoCodecOption.H265,
    val bitrateMbps: Double = 35.0,
    val quality: Int = 80,
    val fecGroup: Int = 6,
    val adaptive: Boolean = true,
    val destIp: String = "",
    val destPort: Int = 5006,
    val destName: String = "",
    val listenPort: Int = 5006,
    val volume: Float = 1.0f,
    val audioLatencyMs: Int = 12,
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
            if (destName.isNotBlank()) destName.take(8) else if (destIp.isNotBlank()) ":${destPort}" else "Пошук"
        } else ":${listenPort}"
        HudTool.AUDIO -> if (mode == BroadcastMode.SENDER) audioSource.title else "${audioLatencyMs} мс"
        HudTool.MONITOR -> if (frameGuides) "Сітка" else "Чисто"
    }
}

/**
 * LanVid Studio bottom dock: segmented tab bar + crisp high-contrast white/red primary GO LIVE action.
 */
@Composable
fun BottomBar(
    ui: HudUiState,
    selectedTool: HudTool?,
    onSelectTool: (HudTool) -> Unit,
    onToggleLive: () -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        color = Studio.hudBar,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(0.75.dp, Studio.borderStrong),
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 8.dp, vertical = 6.dp)
    ) {
        Row(
            Modifier.padding(horizontal = 6.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Row(
                Modifier
                    .weight(1f)
                    .background(Studio.surface2, RoundedCornerShape(10.dp))
                    .border(0.75.dp, Studio.border, RoundedCornerShape(10.dp))
                    .padding(3.dp),
                horizontalArrangement = Arrangement.spacedBy(2.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                HudTool.entries.forEach { t ->
                    val sel = selectedTool == t
                    Column(
                        Modifier
                            .weight(1f)
                            .background(
                                if (sel) Studio.segActive else Color.Transparent,
                                RoundedCornerShape(7.dp)
                            )
                            .clickableNoRipple { onSelectTool(t) }
                            .padding(horizontal = 3.dp, vertical = 5.dp),
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        Text(
                            t.title,
                            style = Bm.sans(9.5.sp, if (sel) FontWeight.SemiBold else FontWeight.Medium),
                            color = if (sel) Studio.text else Studio.text2,
                            maxLines = 1,
                            softWrap = false,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            ui.toolSummary(t),
                            style = Bm.mono(9.sp, FontWeight.Medium),
                            color = if (sel) Studio.accent else Studio.text3,
                            maxLines = 1,
                            softWrap = false,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }
            }

            Spacer(Modifier.width(7.dp))

            val liveBg = when {
                ui.isLive -> Studio.red
                else -> Studio.primaryBg
            }
            val liveFg = if (ui.isLive) Color.White else Studio.primaryFg
            val liveLabel = when {
                ui.isLive -> "Стоп"
                ui.mode == BroadcastMode.RECEIVER -> "Прийом"
                else -> "В ефір"
            }
            Box(
                Modifier
                    .size(width = 76.dp, height = 40.dp)
                    .background(liveBg, RoundedCornerShape(10.dp))
                    .clickableNoRipple(onToggleLive),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    liveLabel,
                    style = Bm.sans(12.sp, FontWeight.SemiBold),
                    color = liveFg,
                    maxLines = 1,
                    softWrap = false
                )
            }
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
            thumbColor = Color.White,
            activeTrackColor = Studio.accent,
            inactiveTrackColor = Studio.surface3
        )
    )
}

@Composable
private fun Row2(title: String, value: String, trailing: @Composable RowScope.() -> Unit = {}) =
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = Bm.sans(12.sp, FontWeight.Medium), color = Studio.text2)
        Spacer(Modifier.width(8.dp))
        Text(value, style = Bm.mono(12.sp, FontWeight.SemiBold), color = Studio.text)
        Spacer(Modifier.weight(1f))
        trailing()
    }

@Composable
private fun SectionHeader(title: String) =
    Text(title, style = Bm.sans(11.5.sp, FontWeight.SemiBold), color = Studio.text2, modifier = Modifier.padding(top = 4.dp))

@Composable
private fun <T> Segments(
    options: List<T>,
    selected: T,
    label: (T) -> String,
    onSelect: (T) -> Unit
) = Row(
    Modifier
        .horizontalScroll(rememberScrollState())
        .background(Studio.surface2, RoundedCornerShape(9.dp))
        .border(0.75.dp, Studio.border, RoundedCornerShape(9.dp))
        .padding(3.dp),
    horizontalArrangement = Arrangement.spacedBy(2.dp)
) {
    options.forEach { o ->
        val sel = o == selected
        Box(
            Modifier
                .background(if (sel) Studio.segActive else Color.Transparent, RoundedCornerShape(7.dp))
                .clickableNoRipple { onSelect(o) }
                .padding(horizontal = 10.dp, vertical = 6.dp)
        ) {
            Text(
                label(o),
                style = Bm.sans(11.5.sp, if (sel) FontWeight.SemiBold else FontWeight.Medium),
                color = if (sel) Studio.text else Studio.text2,
                maxLines = 1
            )
        }
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
        StudioPill(label(v), v == current) { onSelect(v) }
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
        style = Bm.sans(12.5.sp, FontWeight.Medium),
        color = if (enabled) Studio.text else Studio.text3,
        modifier = Modifier.weight(1f)
    )
    Switch(
        checked = checked,
        onCheckedChange = onChange,
        enabled = enabled,
        colors = SwitchDefaults.colors(
            checkedThumbColor = Color.White,
            checkedTrackColor = Studio.accent,
            uncheckedTrackColor = Studio.surface3
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
        Text(label, style = Bm.sans(11.sp, FontWeight.Medium), color = Studio.text2, modifier = Modifier.width(96.dp))
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
        StudioPill("OK", text.trim() != value) { onCommit(text.trim()) }
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
    Surface(
        color = Studio.hudSurface,
        shape = RoundedCornerShape(17.dp),
        border = BorderStroke(0.85.dp, Studio.borderStrong),
        modifier = modifier
            .widthIn(max = 560.dp)
            .heightIn(max = 370.dp)
    ) {
        Column(
            Modifier
                .padding(14.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            when (tool) {
                HudTool.SOURCE -> {
                    SectionHeader("Режим")
                    Segments(BroadcastMode.entries, ui.mode, { if (it == BroadcastMode.SENDER) "Передача (TX)" else "Прийом (RX)" }) {
                        onSwitchMode(it)
                    }
                    if (ui.mode == BroadcastMode.SENDER) {
                        SectionHeader("Камера / Джерело")
                        Segments(CaptureSource.entries, ui.source, { it.title }) {
                            onUpdate(ui.copy(source = it))
                        }
                        SectionHeader("Роздільна здатність")
                        Segments(ResolutionPreset.entries, ui.resolution, { "${it.label} (${it.width}×${it.height})" }) {
                            onUpdate(ui.copy(resolution = it))
                        }
                        SectionHeader("Аудіовхід")
                        Segments(AudioSourceOption.entries, ui.audioSource, { it.title }) {
                            onUpdate(ui.copy(audioSource = it))
                        }
                    } else {
                        Text(
                            "Режим прийому слухає UDP :${ui.listenPort} з AAudio низької затримки.",
                            style = Bm.sans(11.5.sp),
                            color = Studio.text3
                        )
                    }
                }

                HudTool.CODEC -> {
                    SectionHeader("Відеокодек")
                    Segments(VideoCodecOption.entries, ui.codec, { it.title }) {
                        onUpdate(ui.copy(codec = it))
                    }
                    if (ui.codec != VideoCodecOption.DCT) {
                        Row2("Бітрейт ${ui.codec.shortLabel}", "${ui.bitrateMbps.toInt()} Мбіт/с")
                        ValueSlider(ui.bitrateMbps, 1.0..150.0, log = true) {
                            onUpdate(ui.copy(bitrateMbps = Math.round(it).toDouble().coerceIn(1.0, 150.0)))
                        }
                        PresetRow(listOf(15, 35, 50, 80, 100), ui.bitrateMbps.toInt(), { "$it M" }) {
                            onUpdate(ui.copy(bitrateMbps = it.toDouble()))
                        }
                    }
                    Row2(
                        if (ui.codec == VideoCodecOption.DCT) "Якість DCT" else "Цільова якість AIMD",
                        "Q${ui.quality}"
                    )
                    ValueSlider(ui.quality.toDouble(), 30.0..95.0) {
                        onUpdate(ui.copy(quality = Math.round(it).toInt().coerceIn(30, 95)))
                    }
                    Row2("Корекція помилок (XOR FEC)", if (ui.fecGroup == 0) "Вимк." else "${ui.fecGroup} пак.")
                    PresetRow(listOf(0, 4, 6, 8), ui.fecGroup, { if (it == 0) "Вимк." else "FEC $it" }) {
                        onUpdate(ui.copy(fecGroup = it))
                    }
                    SwitchRow("Адаптивна якість (AIMD)", ui.adaptive) {
                        onUpdate(ui.copy(adaptive = it))
                    }
                }

                HudTool.NETWORK -> {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text("Пристрої у мережі", style = Bm.sans(13.5.sp, FontWeight.SemiBold), color = Studio.text)
                        Spacer(Modifier.weight(1f))
                        Chip("v$appVersion · ${simdInfo.uppercase()}", Studio.text2)
                        StudioPill(if (scanning) "Пошук…" else "Оновити", scanning) { onScanLan() }
                    }

                    if (ui.mode == BroadcastMode.SENDER) {
                        if (lanPeers.isNotEmpty()) {
                            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                lanPeers.forEach { p ->
                                    val sel = ui.destIp == p.ip && ui.destPort == p.port
                                    Row(
                                        Modifier
                                            .fillMaxWidth()
                                            .background(
                                                if (sel) Studio.accentSoft else Studio.surface2,
                                                RoundedCornerShape(11.dp)
                                            )
                                            .border(0.75.dp, if (sel) Studio.accent else Studio.border, RoundedCornerShape(11.dp))
                                            .clickableNoRipple {
                                                onUpdate(ui.copy(destIp = p.ip, destPort = p.port, destName = p.name))
                                            }
                                            .padding(horizontal = 10.dp, vertical = 8.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                                    ) {
                                        Chip(p.roleBadge, if (p.role == "video") Studio.green else Studio.accent)
                                        Column(Modifier.weight(1f)) {
                                            Text(p.name, style = Bm.sans(12.5.sp, FontWeight.SemiBold), color = Studio.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                            Text("${p.ip}:${p.port} · ${p.rttMs} мс", style = Bm.mono(10.sp), color = Studio.text3)
                                        }
                                        if (onSelectPeerAndGoLive != null && !ui.senderActive && p.role != "audio") {
                                            Box(
                                                Modifier
                                                    .background(Studio.primaryBg, RoundedCornerShape(8.dp))
                                                    .clickableNoRipple { onSelectPeerAndGoLive(p) }
                                                    .padding(horizontal = 10.dp, vertical = 5.dp)
                                            ) {
                                                Text("В ефір", style = Bm.sans(11.sp, FontWeight.SemiBold), color = Studio.primaryFg)
                                            }
                                        } else if (sel) {
                                            Text("Обрано", style = Bm.sans(11.sp, FontWeight.SemiBold), color = Studio.accent)
                                        }
                                    }
                                }
                            }
                        } else {
                            Text(
                                "Автопошук активний (mDNS + LAN :5023). Відкрийте LanVid Studio на ПК у тій же Wi-Fi мережі.",
                                style = Bm.sans(11.5.sp),
                                color = Studio.text3
                            )
                        }

                        HorizontalDivider(color = Studio.border)
                        TextSetting("IP студії", ui.destIp) { onUpdate(ui.copy(destIp = it, destName = "")) }
                        TextSetting("UDP порт", ui.destPort.toString(), numeric = true) { v ->
                            v.toIntOrNull()?.let { p -> onUpdate(ui.copy(destPort = p.coerceIn(1024, 65535))) }
                        }
                    } else {
                        TextSetting("Порт прийому", ui.listenPort.toString(), numeric = true) { v ->
                            v.toIntOrNull()?.let { p -> onUpdate(ui.copy(listenPort = p.coerceIn(1024, 65535))) }
                        }
                    }
                }

                HudTool.AUDIO -> {
                    SectionHeader("Аудіо та затримка буфера")
                    Row2("Гучність моніторингу", "${(ui.volume * 100).toInt()}%") {
                        StudioPill("100%", false) { onVolumeChange(1.0f) }
                    }
                    ValueSlider(ui.volume.toDouble(), 0.0..1.5) {
                        onVolumeChange(it.toFloat().coerceIn(0f, 1.5f))
                    }
                    Row2("Затримка буфера", "${ui.audioLatencyMs} мс")
                    ValueSlider(ui.audioLatencyMs.toDouble(), 2.0..2000.0, log = true) {
                        onAudioLatencyChange(Math.round(it).toInt().coerceIn(2, 2000))
                    }
                    PresetRow(listOf(4, 12, 28, 50, 200), ui.audioLatencyMs, { "$it мс" }) {
                        onAudioLatencyChange(it)
                    }
                }

                HudTool.MONITOR -> {
                    SectionHeader("Моніторинг та асистенти")
                    SwitchRow("Сітка кадрування", ui.frameGuides) { onUpdate(ui.copy(frameGuides = it)) }
                    SwitchRow("Заповнювати екран (Crop)", ui.previewCropToFill) { onUpdate(ui.copy(previewCropToFill = it)) }
                    SwitchRow("Таймер ефіру", ui.showTimer) { onUpdate(ui.copy(showTimer = it)) }
                    SwitchRow("Телеметрія трафіку", ui.showTraffic) { onUpdate(ui.copy(showTraffic = it)) }
                    HorizontalDivider(color = Studio.border)
                    SectionHeader("Жива телеметрія")
                    if (ui.mode == BroadcastMode.SENDER) {
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Chip("${txStats.width}×${txStats.height}", Studio.text)
                            Chip("%.1f fps".format(txStats.fps), Studio.green)
                            Chip("Кодер %.1f мс".format(txStats.encMs), Studio.violet)
                            Chip("Q${txStats.quality}", Studio.accent)
                            Chip("Аудіо %.0f кбіт/с".format(txStats.audioKbps), Studio.text2)
                        }
                    } else {
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Chip("${rxStats.width}×${rxStats.height}", Studio.text)
                            Chip("%.1f fps".format(rxStats.fps), Studio.green)
                            Chip("E2E %.1f/%.1f мс".format(rxStats.latP50, rxStats.latP99), Studio.violet)
                            Chip("FEC ${rxStats.fecRecovered}", Studio.green)
                            Chip("Втрати %.2f%%".format(rxStats.lossPct), if (rxStats.lossPct > 0.5) Studio.amber else Studio.text)
                        }
                    }
                }
            }
        }
    }
}
