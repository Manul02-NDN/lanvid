package com.lanvid.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.app.StabilizationLevel

/** Full-screen Pro Cine Tally state received from LanVid Studio (PacketType::kTallyState). */
enum class TallyStatus { STANDBY, PREVIEW, PROGRAM }

data class ProCameraState(
    val tally: TallyStatus = TallyStatus.STANDBY,
    val cameraLabel: String = "CAM 1",
    val remoteRec: Boolean = false,
    val directorSpeaking: Boolean = false,
    val duckingGain: Float = 1.0f,
    // Manual Camera Deck & Touch Controls
    val lens: String = "1x",
    val zoomRatio: Float = 1.0f,
    val exposureEv: Float = 0.0f,
    val aeAfLocked: Boolean = false,
    val shutterAngleMode: Boolean = true,
    val shutterAngleDeg: Int = 180,
    val shutterSpeedDenom: Int = 50,
    val shutterLocked: Boolean = false,
    val iso: Int = 400,
    val isoLocked: Boolean = false,
    val wbKelvin: Int = 5600,
    val wbTint: Int = 0,
    val wbLocked: Boolean = false,
    val focusDist: Float = 0.5f,
    val focusLocked: Boolean = false,
    val focusPointA: Float = 0.25f,
    val focusPointB: Float = 0.78f,
    val rackFocusSec: Float = 1.5f,
    val stabilizationLevel: StabilizationLevel = StabilizationLevel.STANDARD,
    val activeStabilization: StabilizationLevel = StabilizationLevel.STANDARD,
    // Viewfinder Assist
    val focusPeaking: Boolean = false,
    val zebraPct: Int = 0,
    val showHistogram: Boolean = true,
    val guideOverlay: String = "16:9",
    // Encoder & Stream Drawer
    val resolution: String = "1080p",
    val fps: String = "60",
    val codec: String = "H.265",
    val tenBit: Boolean = false,
    val cbrMode: Boolean = true,
    val bitrateMbps: Float = 35f,
    val gopMode: String = "1.0s",
    val colorProfile: String = "Rec.709",
    val lutEnabled: Boolean = false,
    // Duplex Intercom (independent of video streaming!)
    val intercomActive: Boolean = false,
    val pttHeld: Boolean = false,
    val latchTalk: Boolean = false,
    val ifbMuted: Boolean = false,
    val ifbGain: Float = 1.0f,
    // Studio Camera Number Badge & Bidirectional Config Sync
    val assignedCamNumber: Int = 0,
    val assignedCamTag: String = "",
    val configSynced: Boolean = false,
    val syncedDelayMs: Int = 33,
    val syncedFixedDelay: Boolean = true,
    val lastSyncRev: Long = 0L,
) {
    val badgeDisplayText: String
        get() = assignedCamTag.ifBlank {
            if (assignedCamNumber > 0) "CAM $assignedCamNumber" else cameraLabel.take(8).ifBlank { "CAM 1" }
        }
}

/**
 * Clean neutral LanVid Studio sub-bar (Tally pill + quick Camera / Quality drawer toggles).
 */
@Composable
fun ProSubTopBar(
    state: ProCameraState,
    showManualDeck: Boolean,
    showEncoderDrawer: Boolean,
    onToggleManual: () -> Unit,
    onToggleEncoder: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier
            .fillMaxWidth()
            .padding(horizontal = 10.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val dotColor = when (state.tally) {
            TallyStatus.PROGRAM -> Studio.red
            TallyStatus.PREVIEW -> Studio.green
            TallyStatus.STANDBY -> Studio.text3
        }
        val tallyText = when (state.tally) {
            TallyStatus.PROGRAM -> "PGM"
            TallyStatus.PREVIEW -> "PVW"
            TallyStatus.STANDBY -> "STBY"
        }
        StudioStatusPill(
            dotColor = dotColor,
            title = "${state.badgeDisplayText} · $tallyText",
            detail = "${state.stabilizationLevel.title} · ${"%.1f".format(state.zoomRatio)}×"
        )

        Spacer(Modifier.weight(1f))

        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (state.directorSpeaking) {
                Surface(color = Studio.accentSoft, shape = RoundedCornerShape(8.dp), border = BorderStroke(0.75.dp, Studio.accent)) {
                    Text(
                        "Режисер",
                        Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        color = Studio.accent,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 10.5.sp,
                        maxLines = 1,
                    )
                }
            } else if (state.intercomActive) {
                Surface(color = Studio.greenSoft, shape = RoundedCornerShape(8.dp), border = BorderStroke(0.75.dp, Studio.green)) {
                    Text(
                        "IFB",
                        Modifier.padding(horizontal = 7.dp, vertical = 4.dp),
                        color = Studio.green,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 10.sp,
                        maxLines = 1,
                    )
                }
            }
            if (state.remoteRec) {
                Surface(color = Studio.red, shape = RoundedCornerShape(8.dp)) {
                    Text(
                        "● REC",
                        Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        color = Color.White,
                        fontWeight = FontWeight.Bold,
                        fontSize = 10.sp,
                        maxLines = 1,
                    )
                }
            }
            StudioPill("Камера", showManualDeck, onToggleManual)
            StudioPill("Якість", showEncoderDrawer, onToggleEncoder)
        }
    }
}

/**
 * LanVid Studio Camera & Multi-Level Stabilization Drawer.
 */
@Composable
fun ProManualDeckDrawer(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    onStabilizationSelected: (StabilizationLevel) -> Unit,
    onExposureEvChange: (Float) -> Unit,
    onZoomRatioChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        color = Studio.hudSurface,
        shape = RoundedCornerShape(17.dp),
        border = BorderStroke(0.85.dp, Studio.borderStrong),
        modifier = modifier
            .padding(horizontal = 10.dp, vertical = 6.dp)
            .widthIn(max = 350.dp)
            .heightIn(max = 390.dp),
    ) {
        Column(
            Modifier.padding(14.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Text("Камера і стабілізація", color = Studio.text, fontWeight = FontWeight.SemiBold, fontSize = 13.5.sp)
                Chip("HW: ${state.activeStabilization.title}", if (state.activeStabilization == StabilizationLevel.OFF) Studio.text3 else Studio.green)
            }

            // 5-Level Image Stabilization Selector
            Column(verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Text("Стабілізація зображення", color = Studio.text2, fontSize = 11.5.sp, fontWeight = FontWeight.Medium)
                Row(
                    Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(5.dp)
                ) {
                    StabilizationLevel.entries.forEach { lvl ->
                        StudioPill(lvl.title, state.stabilizationLevel == lvl) {
                            onStabilizationSelected(lvl)
                        }
                    }
                }
                Text(state.stabilizationLevel.subtitle, color = Studio.text3, fontSize = 10.5.sp)
            }

            HorizontalDivider(color = Studio.border)

            // Lens + Continuous Zoom
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Об'єктив і зум", color = Studio.text2, fontSize = 11.5.sp)
                Text("${"%.1f".format(state.zoomRatio)}×", style = Bm.mono(11.5.sp), color = Studio.accent)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("0.5x" to 0.6f, "1x" to 1.0f, "2x" to 2.0f, "3x" to 3.0f).forEach { (lbl, z) ->
                    StudioPill(lbl, state.lens == lbl) {
                        onStateChange(state.copy(lens = lbl, zoomRatio = z))
                        onZoomRatioChange(z)
                    }
                }
                StudioPill("Фронт", state.lens == "Front") {
                    onStateChange(state.copy(lens = "Front", zoomRatio = 1.0f))
                }
            }
            Slider(
                value = state.zoomRatio.coerceIn(1f, 8f),
                onValueChange = {
                    onStateChange(state.copy(zoomRatio = it))
                    onZoomRatioChange(it)
                },
                valueRange = 1f..8f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.accent, inactiveTrackColor = Studio.surface3),
            )

            HorizontalDivider(color = Studio.border)

            // EV Exposure Compensation
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Експозиція (EV)", color = Studio.text2, fontSize = 11.5.sp)
                Text("%+.1f EV".format(state.exposureEv), style = Bm.mono(11.5.sp), color = Studio.yellow)
            }
            Slider(
                value = state.exposureEv,
                onValueChange = {
                    onStateChange(state.copy(exposureEv = it))
                    onExposureEvChange(it)
                },
                valueRange = -3f..3f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.accent, inactiveTrackColor = Studio.surface3),
            )

            // Shutter & ISO
            Text(
                if (state.shutterAngleMode) "Затвор: ${state.shutterAngleDeg}°"
                else "Затвор: 1/${state.shutterSpeedDenom}s",
                color = Studio.text2, fontSize = 11.5.sp,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf(45, 90, 180, 360).forEach { ang ->
                    StudioPill("${ang}°", state.shutterAngleDeg == ang && state.shutterLocked) {
                        onStateChange(state.copy(shutterAngleMode = true, shutterAngleDeg = ang, shutterLocked = true))
                    }
                }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("ISO ${state.iso}", style = Bm.mono(11.5.sp), color = Studio.text)
                Text("WB ${state.wbKelvin}K", style = Bm.mono(11.5.sp), color = Studio.text2)
            }
            Slider(
                value = state.iso.toFloat(),
                onValueChange = { onStateChange(state.copy(iso = it.toInt())) },
                valueRange = 50f..6400f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.accent, inactiveTrackColor = Studio.surface3),
            )
            Slider(
                value = state.wbKelvin.toFloat(),
                onValueChange = { onStateChange(state.copy(wbKelvin = it.toInt())) },
                valueRange = 2500f..10000f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.amber, inactiveTrackColor = Studio.surface3),
            )

            // Manual Focus + Rack A <-> B
            Text("Фокус: ${(state.focusDist * 100).toInt()}% (Rack A↔B)", color = Studio.text2, fontSize = 11.5.sp)
            Slider(
                value = state.focusDist,
                onValueChange = { onStateChange(state.copy(focusDist = it)) },
                valueRange = 0f..1f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.accent, inactiveTrackColor = Studio.surface3),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                StudioPill("Set A", false) { onStateChange(state.copy(focusPointA = state.focusDist)) }
                StudioPill("Set B", false) { onStateChange(state.copy(focusPointB = state.focusDist)) }
                StudioPill("A ↔ B", false) {
                    val target = if (kotlin.math.abs(state.focusDist - state.focusPointA) < 0.05f) state.focusPointB else state.focusPointA
                    onStateChange(state.copy(focusDist = target))
                }
                StudioPill("Пікінг", state.focusPeaking) { onStateChange(state.copy(focusPeaking = !state.focusPeaking)) }
            }
        }
    }
}

/**
 * LanVid Studio Quality & Transmission Drawer (matches ref4 styling).
 */
@Composable
fun ProEncoderDrawer(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        color = Studio.hudSurface,
        shape = RoundedCornerShape(17.dp),
        border = BorderStroke(0.85.dp, Studio.borderStrong),
        modifier = modifier
            .padding(horizontal = 10.dp, vertical = 6.dp)
            .widthIn(max = 340.dp)
            .heightIn(max = 370.dp),
    ) {
        Column(
            Modifier.padding(14.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(9.dp),
        ) {
            Text("Якість і передача", color = Studio.text, fontWeight = FontWeight.SemiBold, fontSize = 13.5.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("720p", "1080p", "4K UHD").forEach { r ->
                    StudioPill(r, state.resolution == r) { onStateChange(state.copy(resolution = r)) }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("24", "30", "60").forEach { f ->
                    StudioPill("${f}p", state.fps == f) { onStateChange(state.copy(fps = f)) }
                }
                Spacer(Modifier.width(4.dp))
                listOf("H.265", "H.264", "DCT").forEach { c ->
                    StudioPill(c, state.codec == c) { onStateChange(state.copy(codec = c)) }
                }
            }
            HorizontalDivider(color = Studio.border)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Бітрейт потоку", color = Studio.text2, fontSize = 12.sp)
                Text("${state.bitrateMbps.toInt()} Мбіт/с", style = Bm.mono(12.sp), color = Studio.text)
            }
            Slider(
                value = state.bitrateMbps,
                onValueChange = { onStateChange(state.copy(bitrateMbps = it)) },
                valueRange = 5f..150f,
                colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Studio.accent, inactiveTrackColor = Studio.surface3),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                StudioPill("Wi-Fi 15M", state.bitrateMbps.toInt() == 15) { onStateChange(state.copy(bitrateMbps = 15f)) }
                StudioPill("Студія 35M", state.bitrateMbps.toInt() == 35) { onStateChange(state.copy(bitrateMbps = 35f)) }
                StudioPill("Майстер 80M", state.bitrateMbps.toInt() == 80) { onStateChange(state.copy(bitrateMbps = 80f)) }
            }
            HorizontalDivider(color = Studio.border)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("Rec.709", "Flat", "Log").forEach { p ->
                    StudioPill(p, state.colorProfile == p) { onStateChange(state.copy(colorProfile = p)) }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                Text("10-bit Color / LUT", color = Studio.text2, fontSize = 12.sp)
                Switch(
                    checked = state.lutEnabled,
                    onCheckedChange = { onStateChange(state.copy(lutEnabled = it)) },
                    colors = SwitchDefaults.colors(checkedThumbColor = Color.White, checkedTrackColor = Studio.accent)
                )
            }
        }
    }
}

/**
 * Compact LanVid Studio Duplex Intercom Strip (Independent ON/OFF / PTT / LATCH / IFB).
 */
@Composable
fun ProIntercomStrip(
    state: ProCameraState,
    onToggleIntercom: () -> Unit,
    onEnsureIntercomActive: () -> Boolean,
    onStateChange: (ProCameraState) -> Unit,
    onIntercomChanged: (talk: Boolean, listenMuted: Boolean, rxGain: Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val currentState by rememberUpdatedState(state)
    val currentEnsureActive by rememberUpdatedState(onEnsureIntercomActive)
    val currentOnStateChange by rememberUpdatedState(onStateChange)
    val currentOnIntercomChanged by rememberUpdatedState(onIntercomChanged)

    Box(modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Surface(
            color = Studio.hudBar,
            shape = RoundedCornerShape(999.dp),
            border = BorderStroke(0.75.dp, if (state.intercomActive) Studio.green.copy(alpha = 0.65f) else Studio.borderStrong),
        ) {
            Row(
                Modifier
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = 8.dp, vertical = 5.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // Dedicated independent Intercom power toggle
                Surface(
                    onClick = onToggleIntercom,
                    color = if (state.intercomActive) Studio.greenSoft else Studio.surface2,
                    shape = RoundedCornerShape(999.dp),
                    border = BorderStroke(0.75.dp, if (state.intercomActive) Studio.green else Studio.border),
                ) {
                    Row(
                        Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                        horizontalArrangement = Arrangement.spacedBy(5.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Box(
                            Modifier
                                .size(6.dp)
                                .clip(CircleShape)
                                .background(if (state.intercomActive) Studio.green else Studio.text3)
                        )
                        Text(
                            if (state.intercomActive) "Інтерком" else "Інтерком Вимк",
                            color = if (state.intercomActive) Studio.green else Studio.text2,
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 11.sp,
                            maxLines = 1,
                        )
                    }
                }

                val talking = state.intercomActive && (state.pttHeld || state.latchTalk)
                Box(
                    Modifier
                        .clip(RoundedCornerShape(999.dp))
                        .background(if (talking) Studio.red else Studio.surface2)
                        .pointerInput(Unit) {
                            detectTapGestures(
                                onPress = {
                                    val st = currentState
                                    if (st.intercomActive || currentEnsureActive()) {
                                        val active = currentState
                                        currentOnStateChange(active.copy(intercomActive = true, pttHeld = true))
                                        currentOnIntercomChanged(true, active.ifbMuted, active.ifbGain)
                                        tryAwaitRelease()
                                        val after = currentState
                                        currentOnStateChange(after.copy(pttHeld = false))
                                        if (after.intercomActive) {
                                            currentOnIntercomChanged(after.latchTalk, after.ifbMuted, after.ifbGain)
                                        }
                                    }
                                }
                            )
                        }
                        .padding(horizontal = 12.dp, vertical = 5.dp)
                ) {
                    Text(
                        if (talking) "Мікрофон ●" else "PTT",
                        color = if (talking) Color.White else if (state.intercomActive) Studio.text else Studio.text2,
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 11.sp,
                        maxLines = 1,
                    )
                }

                StudioPill("Latch", state.intercomActive && state.latchTalk) {
                    if (!state.intercomActive) {
                        if (onEnsureIntercomActive()) {
                            onStateChange(state.copy(intercomActive = true, latchTalk = true))
                            onIntercomChanged(true, state.ifbMuted, state.ifbGain)
                        }
                    } else {
                        val nxt = !state.latchTalk
                        onStateChange(state.copy(latchTalk = nxt))
                        onIntercomChanged(nxt || state.pttHeld, state.ifbMuted, state.ifbGain)
                    }
                }

                val ifbListening = state.intercomActive && !state.ifbMuted
                StudioPill(if (ifbListening) "IFB Увімк" else "IFB Вимк", ifbListening) {
                    if (!state.intercomActive) {
                        if (onEnsureIntercomActive()) {
                            onStateChange(state.copy(intercomActive = true, ifbMuted = false))
                            onIntercomChanged(state.latchTalk || state.pttHeld, false, state.ifbGain)
                        }
                    } else {
                        val nxtMuted = !state.ifbMuted
                        onStateChange(state.copy(ifbMuted = nxtMuted))
                        onIntercomChanged(state.latchTalk || state.pttHeld, nxtMuted, state.ifbGain)
                    }
                }
            }
        }
    }
}

/**
 * Compact neutral LanVid Studio camera badge with Tally dot and Studio sync tooltip.
 */
@Composable
fun CameraNumberBadge(
    state: ProCameraState,
    onOpenNetwork: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var showTooltip by remember { mutableStateOf(false) }
    val dotColor = when (state.tally) {
        TallyStatus.PROGRAM -> Studio.red
        TallyStatus.PREVIEW -> Studio.green
        TallyStatus.STANDBY -> Studio.accent
    }
    Column(modifier = modifier, horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Surface(
            onClick = { showTooltip = !showTooltip },
            color = Studio.hudBar,
            shape = RoundedCornerShape(10.dp),
            border = BorderStroke(1.dp, if (state.tally == TallyStatus.STANDBY) Studio.borderStrong else dotColor),
            modifier = Modifier.heightIn(min = 32.dp),
        ) {
            Row(
                Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(Modifier.size(7.dp).clip(CircleShape).background(dotColor))
                Text(
                    state.badgeDisplayText,
                    color = Studio.text,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 11.5.sp,
                    maxLines = 1,
                )
            }
        }
        if (showTooltip) {
            Surface(
                color = Studio.hudSurface,
                shape = RoundedCornerShape(11.dp),
                border = BorderStroke(0.85.dp, Studio.borderStrong),
            ) {
                Row(
                    Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val modeTxt = if (state.syncedFixedDelay) "${state.syncedDelayMs} мс" else "Авто"
                    val msg = if (state.configSynced) {
                        "Студія: ${state.badgeDisplayText} · $modeTxt"
                    } else {
                        "Камера: ${state.badgeDisplayText} · Очікування"
                    }
                    Text(msg, color = Studio.text2, fontSize = 10.5.sp, fontWeight = FontWeight.Medium)
                    StudioPill("Мережа", false) {
                        showTooltip = false
                        onOpenNetwork()
                    }
                }
            }
        }
    }
}

@Composable
fun StudioPill(label: String, active: Boolean, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        color = if (active) Studio.accentSoft else Studio.surface2,
        shape = RoundedCornerShape(999.dp),
        border = BorderStroke(0.9.dp, if (active) Studio.accent else Studio.borderStrong),
    ) {
        Text(
            label,
            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            color = if (active) Studio.accent else Studio.text2,
            fontWeight = if (active) FontWeight.SemiBold else FontWeight.Medium,
            fontSize = 11.sp,
            maxLines = 1,
        )
    }
}
