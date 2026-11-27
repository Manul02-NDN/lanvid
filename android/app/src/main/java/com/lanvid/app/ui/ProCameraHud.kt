package com.lanvid.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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

/** Full-screen Pro Cine Tally state received from LanVid Studio (PacketType::kTallyState). */
enum class TallyStatus { STANDBY, PREVIEW, PROGRAM }

data class ProCameraState(
    val tally: TallyStatus = TallyStatus.STANDBY,
    val cameraLabel: String = "CAM 1 • Wide",
    val remoteRec: Boolean = false,
    val directorSpeaking: Boolean = false,
    val duckingGain: Float = 1.0f,
    // Manual Camera Deck
    val lens: String = "1x",
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
    val stabilization: String = "Standard",
    // Viewfinder Assist
    val focusPeaking: Boolean = false,
    val zebraPct: Int = 0,  // 0=Off, 70, 95
    val showHistogram: Boolean = true,
    val guideOverlay: String = "16:9",
    // Encoder & Stream Drawer
    val resolution: String = "1080p",
    val fps: String = "60",
    val codec: String = "H.265",
    val tenBit: Boolean = false,
    val cbrMode: Boolean = true,
    val bitrateMbps: Float = 25f,
    val gopMode: String = "1.0s",
    val colorProfile: String = "Rec.709",
    val lutEnabled: Boolean = false,
    // Duplex Intercom
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

private val CineGlass = Color(0xCC18122B)
private val CineLavender = Color(0xFFDDD6FE)
private val CineAccent = Color(0xFF8B5CF6)
private val CineGreen = Color(0xFF10B981)
private val CineRed = Color(0xFFEF4444)
private val CineMuted = Color(0xFFA79ECC)

/**
 * Sub-Top Bar placed cleanly below TopHudBar inside the unified HUD Column (never overlaps status bar or cutout).
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
        val tallyBg = when (state.tally) {
            TallyStatus.PROGRAM -> CineRed
            TallyStatus.PREVIEW -> CineGreen
            TallyStatus.STANDBY -> CineGlass
        }
        val tallyText = when (state.tally) {
            TallyStatus.PROGRAM -> "PGM"
            TallyStatus.PREVIEW -> "PVW"
            TallyStatus.STANDBY -> "STBY"
        }
        Surface(
            color = tallyBg,
            shape = RoundedCornerShape(16.dp),
            modifier = Modifier.weight(1f, fill = false),
        ) {
            Row(
                Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.size(7.dp).clip(CircleShape).background(Color.White))
                Spacer(Modifier.width(6.dp))
                Text(
                    "${state.cameraLabel} · $tallyText",
                    color = Color.White,
                    fontWeight = FontWeight.Bold,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        Spacer(Modifier.weight(1f))

        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (state.directorSpeaking) {
                Surface(color = CineAccent, shape = RoundedCornerShape(14.dp)) {
                    Text(
                        "DIR IFB",
                        Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        color = Color.White,
                        fontWeight = FontWeight.Bold,
                        fontSize = 10.sp,
                        maxLines = 1,
                    )
                }
            }
            if (state.remoteRec) {
                Surface(color = CineRed, shape = RoundedCornerShape(14.dp)) {
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
            CinePill("MANUAL", showManualDeck, onToggleManual)
            CinePill("ENC", showEncoderDrawer, onToggleEncoder)
        }
    }
}

/**
 * Slide-out Manual Camera Deck rendered inside the center viewfinder Box (never overlaps top/bottom HUD).
 */
@Composable
fun ProManualDeckDrawer(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        color = CineGlass,
        shape = RoundedCornerShape(18.dp),
        border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.35f)),
        modifier = modifier
            .padding(horizontal = 10.dp, vertical = 6.dp)
            .widthIn(max = 300.dp)
            .heightIn(max = 360.dp),
    ) {
        Column(
            Modifier.padding(12.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("MANUAL CAMERA DECK", color = CineLavender, fontWeight = FontWeight.Bold, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("0.5x", "1x", "2x", "Front").forEach { l ->
                    CinePill(l, state.lens == l) { onStateChange(state.copy(lens = l)) }
                }
            }
            Text(
                if (state.shutterAngleMode) "Shutter Angle: ${state.shutterAngleDeg}°"
                else "Shutter: 1/${state.shutterSpeedDenom}s",
                color = Color.White, fontSize = 12.sp,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf(45, 90, 180, 360).forEach { ang ->
                    CinePill("${ang}°", state.shutterAngleDeg == ang) {
                        onStateChange(state.copy(shutterAngleMode = true, shutterAngleDeg = ang))
                    }
                }
            }
            Text("ISO: ${state.iso}", color = Color.White, fontSize = 12.sp)
            Slider(
                value = state.iso.toFloat(),
                onValueChange = { onStateChange(state.copy(iso = it.toInt())) },
                valueRange = 50f..6400f,
                colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineAccent),
            )
            Text("WB: ${state.wbKelvin}K · Tint ${state.wbTint}", color = Color.White, fontSize = 12.sp)
            Slider(
                value = state.wbKelvin.toFloat(),
                onValueChange = { onStateChange(state.copy(wbKelvin = it.toInt())) },
                valueRange = 2500f..10000f,
                colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineAccent),
            )
            Text("Focus: ${(state.focusDist * 100).toInt()}% (Rack A↔B ${state.rackFocusSec}s)", color = Color.White, fontSize = 12.sp)
            Slider(
                value = state.focusDist,
                onValueChange = { onStateChange(state.copy(focusDist = it)) },
                valueRange = 0f..1f,
                colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineGreen),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                CinePill("Set A", false) { onStateChange(state.copy(focusPointA = state.focusDist)) }
                CinePill("Set B", false) { onStateChange(state.copy(focusPointB = state.focusDist)) }
                CinePill("Go A↔B", false) {
                    val target = if (kotlin.math.abs(state.focusDist - state.focusPointA) < 0.05f) state.focusPointB else state.focusPointA
                    onStateChange(state.copy(focusDist = target))
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                CinePill("Peaking", state.focusPeaking) { onStateChange(state.copy(focusPeaking = !state.focusPeaking)) }
                CinePill("Zebra ${if (state.zebraPct == 0) "Off" else "${state.zebraPct}%"}", state.zebraPct > 0) {
                    val nxt = when (state.zebraPct) { 0 -> 70; 70 -> 95; else -> 0 }
                    onStateChange(state.copy(zebraPct = nxt))
                }
                CinePill("OIS: ${state.stabilization}", state.stabilization != "Off") {
                    val nxt = when (state.stabilization) { "Off" -> "Standard"; "Standard" -> "Active"; else -> "Off" }
                    onStateChange(state.copy(stabilization = nxt))
                }
            }
        }
    }
}

/**
 * Slide-out Encoder & Stream Drawer rendered inside the center viewfinder Box.
 */
@Composable
fun ProEncoderDrawer(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        color = CineGlass,
        shape = RoundedCornerShape(18.dp),
        border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.35f)),
        modifier = modifier
            .padding(horizontal = 10.dp, vertical = 6.dp)
            .widthIn(max = 300.dp)
            .heightIn(max = 360.dp),
    ) {
        Column(
            Modifier.padding(12.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("ENCODER & STREAM DRAWER", color = CineLavender, fontWeight = FontWeight.Bold, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("4K UHD", "1080p", "720p").forEach { r ->
                    CinePill(r, state.resolution == r) { onStateChange(state.copy(resolution = r)) }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("24", "25", "30", "50", "60").forEach { f ->
                    CinePill("${f}p", state.fps == f) { onStateChange(state.copy(fps = f)) }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("H.265", "H.264", "DCT").forEach { c ->
                    CinePill(c, state.codec == c) { onStateChange(state.copy(codec = c)) }
                }
            }
            Text("Bitrate: ${state.bitrateMbps.toInt()} Mbps (${if (state.cbrMode) "CBR" else "VBR"} · Zero B-Frames)", color = Color.White, fontSize = 12.sp)
            Slider(
                value = state.bitrateMbps,
                onValueChange = { onStateChange(state.copy(bitrateMbps = it)) },
                valueRange = 5f..150f,
                colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineAccent),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                CinePill("Wi-Fi 15M", state.bitrateMbps.toInt() == 15) { onStateChange(state.copy(bitrateMbps = 15f)) }
                CinePill("Studio 50M", state.bitrateMbps.toInt() == 50) { onStateChange(state.copy(bitrateMbps = 50f)) }
                CinePill("Master 120M", state.bitrateMbps.toInt() == 120) { onStateChange(state.copy(bitrateMbps = 120f)) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("Rec.709", "Flat", "Log").forEach { p ->
                    CinePill(p, state.colorProfile == p) { onStateChange(state.copy(colorProfile = p)) }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween, modifier = Modifier.fillMaxWidth()) {
                Text("10-bit Color / .cube LUT", color = CineMuted, fontSize = 12.sp)
                Switch(checked = state.lutEnabled, onCheckedChange = { onStateChange(state.copy(lutEnabled = it)) })
            }
        }
    }
}

/**
 * Compact Duplex Intercom Strip (TALK PTT / LATCH / IFB ON) placed sequentially above telemetry in the HUD Column.
 */
@Composable
fun ProIntercomStrip(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    onIntercomChanged: (talk: Boolean, listenMuted: Boolean, rxGain: Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Surface(
            color = CineGlass,
            shape = RoundedCornerShape(20.dp),
            border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.3f)),
        ) {
            Row(
                Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val talking = state.pttHeld || state.latchTalk
                Box(
                    Modifier
                        .clip(RoundedCornerShape(14.dp))
                        .background(if (talking) CineRed else CineAccent)
                        .pointerInput(state.latchTalk, state.ifbMuted, state.ifbGain) {
                            detectTapGestures(
                                onPress = {
                                    onStateChange(state.copy(pttHeld = true))
                                    onIntercomChanged(true, state.ifbMuted, state.ifbGain)
                                    tryAwaitRelease()
                                    onStateChange(state.copy(pttHeld = false))
                                    onIntercomChanged(state.latchTalk, state.ifbMuted, state.ifbGain)
                                }
                            )
                        }
                        .padding(horizontal = 14.dp, vertical = 6.dp)
                ) {
                    Text(
                        if (talking) "TALKING ●" else "TALK (PTT)",
                        color = Color.White,
                        fontWeight = FontWeight.Bold,
                        fontSize = 11.sp,
                        maxLines = 1,
                    )
                }

                CinePill("LATCH", state.latchTalk) {
                    val nxt = !state.latchTalk
                    onStateChange(state.copy(latchTalk = nxt))
                    onIntercomChanged(nxt || state.pttHeld, state.ifbMuted, state.ifbGain)
                }

                CinePill(if (state.ifbMuted) "IFB MUTE" else "IFB ON", !state.ifbMuted) {
                    val nxtMuted = !state.ifbMuted
                    onStateChange(state.copy(ifbMuted = nxtMuted))
                    onIntercomChanged(state.latchTalk || state.pttHeld, nxtMuted, state.ifbGain)
                }
            }
        }
    }
}

/**
 * Compact glass badge showing the studio-assigned camera number ("CAM 1", "CAM 2", "A"...)
 * with dynamic Tally border glow (Lavender STBY / Emerald PVW / Red PGM) and tap tooltip.
 */
@Composable
fun CameraNumberBadge(
    state: ProCameraState,
    onOpenNetwork: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var showTooltip by remember { mutableStateOf(false) }
    val borderColor = when (state.tally) {
        TallyStatus.PROGRAM -> CineRed
        TallyStatus.PREVIEW -> CineGreen
        TallyStatus.STANDBY -> CineAccent
    }
    Column(modifier = modifier, horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Surface(
            onClick = { showTooltip = !showTooltip },
            color = CineGlass,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(2.dp, borderColor),
            shadowElevation = 4.dp,
            modifier = Modifier.heightIn(min = 36.dp),
        ) {
            Row(
                Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(Modifier.size(7.dp).clip(CircleShape).background(borderColor))
                Text(
                    state.badgeDisplayText,
                    color = Color.White,
                    fontWeight = FontWeight.ExtraBold,
                    fontSize = 12.sp,
                    maxLines = 1,
                )
            }
        }
        if (showTooltip) {
            Surface(
                color = CineGlass,
                shape = RoundedCornerShape(12.dp),
                border = BorderStroke(1.dp, borderColor.copy(alpha = 0.7f)),
            ) {
                Row(
                    Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val modeTxt = if (state.syncedFixedDelay) "${state.syncedDelayMs} ms" else "Adaptive"
                    val msg = if (state.configSynced) {
                        "Призначено студією: ${state.badgeDisplayText} • Synced $modeTxt"
                    } else {
                        "Камера: ${state.badgeDisplayText} • Очікування синхронізації"
                    }
                    Text(msg, color = CineLavender, fontSize = 10.sp, fontWeight = FontWeight.Medium)
                    CinePill("Мережа", false) {
                        showTooltip = false
                        onOpenNetwork()
                    }
                }
            }
        }
    }
}

@Composable
private fun CinePill(label: String, active: Boolean, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        color = if (active) CineAccent else Color(0x99241B44),
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, if (active) CineLavender else Color(0x44DDD6FE)),
    ) {
        Text(
            label,
            Modifier.padding(horizontal = 9.dp, vertical = 5.dp),
            color = Color.White,
            fontWeight = if (active) FontWeight.Bold else FontWeight.Medium,
            fontSize = 10.sp,
            maxLines = 1,
        )
    }
}
