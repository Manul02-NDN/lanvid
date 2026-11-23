package com.lanvid.app.ui

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
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
    val fps: String = "30",
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
)

private val CineGlass = Color(0xCC18122B)
private val CineLavender = Color(0xFFDDD6FE)
private val CineAccent = Color(0xFF8B5CF6)
private val CineGreen = Color(0xFF10B981)
private val CineRed = Color(0xFFEF4444)
private val CineMuted = Color(0xFFA79ECC)

/**
 * Soft Pastel Lavender / Dark Glass Pro Cine HUD overlay:
 *  - Full-screen 5 dp Tally border (PGM red pulsing / PVW green / STBY neutral)
 *  - Top status pill (Camera label, Tally, Director Speaking IFB badge, REC)
 *  - Bottom Manual Camera Deck + Duplex Intercom PTT/Latch/IFB widget
 *  - Slide-out Encoder & Stream Drawer
 */
@Composable
fun ProCameraHudOverlay(
    state: ProCameraState,
    onStateChange: (ProCameraState) -> Unit,
    onIntercomChanged: (talk: Boolean, listenMuted: Boolean, rxGain: Float) -> Unit,
    modifier: Modifier = Modifier,
) {
    val pulse = rememberInfiniteTransition(label = "tallyPulse")
    val pulseAlpha by pulse.animateFloat(
        initialValue = 0.55f,
        targetValue = 1.0f,
        animationSpec = infiniteRepeatable(tween(550), RepeatMode.Reverse),
        label = "pulseAlpha",
    )

    val borderCol = when (state.tally) {
        TallyStatus.PROGRAM -> CineRed.copy(alpha = pulseAlpha)
        TallyStatus.PREVIEW -> CineGreen
        TallyStatus.STANDBY -> Color.Transparent
    }

    var showManualDeck by remember { mutableStateOf(false) }
    var showEncoderDrawer by remember { mutableStateOf(false) }

    Box(
        modifier = modifier
            .fillMaxSize()
            .then(
                if (state.tally != TallyStatus.STANDBY) Modifier.border(5.dp, borderCol) else Modifier
            )
    ) {
        // ---- TOP TALLY & DIRECTOR IFB BAR ----
        Row(
            Modifier
                .fillMaxWidth()
                .align(Alignment.TopCenter)
                .padding(horizontal = 12.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val tallyBg = when (state.tally) {
                TallyStatus.PROGRAM -> CineRed
                TallyStatus.PREVIEW -> CineGreen
                TallyStatus.STANDBY -> CineGlass
            }
            val tallyText = when (state.tally) {
                TallyStatus.PROGRAM -> "PGM • ON AIR"
                TallyStatus.PREVIEW -> "PVW • PREVIEW"
                TallyStatus.STANDBY -> "STBY"
            }
            Surface(color = tallyBg, shape = RoundedCornerShape(18.dp)) {
                Row(
                    Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(Color.White))
                    Spacer(Modifier.width(6.dp))
                    Text("${state.cameraLabel} · $tallyText", color = Color.White, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                if (state.directorSpeaking) {
                    Surface(color = CineAccent, shape = RoundedCornerShape(16.dp)) {
                        Text(
                            "DIRECTOR SPEAKING",
                            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                            color = Color.White,
                            fontWeight = FontWeight.Bold,
                            fontSize = 11.sp,
                        )
                    }
                }
                if (state.remoteRec) {
                    Surface(color = CineRed, shape = RoundedCornerShape(16.dp)) {
                        Text(
                            "● REC",
                            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
                            color = Color.White,
                            fontWeight = FontWeight.Bold,
                            fontSize = 11.sp,
                        )
                    }
                }
                CinePill("MANUAL", showManualDeck) {
                    showManualDeck = !showManualDeck
                    if (showManualDeck) showEncoderDrawer = false
                }
                CinePill("ENC", showEncoderDrawer) {
                    showEncoderDrawer = !showEncoderDrawer
                    if (showEncoderDrawer) showManualDeck = false
                }
            }
        }

        // ---- SLIDE-OUT MANUAL CAMERA DECK ----
        if (showManualDeck) {
            Surface(
                color = CineGlass,
                shape = RoundedCornerShape(20.dp),
                border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.35f)),
                modifier = Modifier
                    .align(Alignment.CenterStart)
                    .padding(12.dp)
                    .width(300.dp),
            ) {
                Column(
                    Modifier.padding(12.dp).verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text("MANUAL CAMERA DECK", color = CineLavender, fontWeight = FontWeight.Bold, fontSize = 12.sp)
                    // Lens Selector
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        listOf("0.5x", "1x", "2x", "Front").forEach { l ->
                            CinePill(l, state.lens == l) { onStateChange(state.copy(lens = l)) }
                        }
                    }
                    // Shutter Angle / Speed
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
                    // ISO
                    Text("ISO: ${state.iso}", color = Color.White, fontSize = 12.sp)
                    Slider(
                        value = state.iso.toFloat(),
                        onValueChange = { onStateChange(state.copy(iso = it.toInt())) },
                        valueRange = 50f..6400f,
                        colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineAccent),
                    )
                    // White Balance (Kelvin + Tint)
                    Text("WB: ${state.wbKelvin}K · Tint ${state.wbTint}", color = Color.White, fontSize = 12.sp)
                    Slider(
                        value = state.wbKelvin.toFloat(),
                        onValueChange = { onStateChange(state.copy(wbKelvin = it.toInt())) },
                        valueRange = 2500f..10000f,
                        colors = SliderDefaults.colors(thumbColor = CineLavender, activeTrackColor = CineAccent),
                    )
                    // Manual Focus + A <-> B Rack Focus
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
                    // Viewfinder Assist & OIS
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

        // ---- SLIDE-OUT ENCODER & STREAM DRAWER ----
        if (showEncoderDrawer) {
            Surface(
                color = CineGlass,
                shape = RoundedCornerShape(20.dp),
                border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.35f)),
                modifier = Modifier
                    .align(Alignment.CenterEnd)
                    .padding(12.dp)
                    .width(300.dp),
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

        // ---- BOTTOM DUPLEX INTERCOM HUD (PTT / LATCH / IFB) ----
        Surface(
            color = CineGlass,
            shape = RoundedCornerShape(22.dp),
            border = BorderStroke(1.dp, CineLavender.copy(alpha = 0.3f)),
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(horizontal = 14.dp, vertical = 12.dp),
        ) {
            Row(
                Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                val talking = state.pttHeld || state.latchTalk
                Box(
                    Modifier
                        .clip(RoundedCornerShape(16.dp))
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
                        .padding(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Text(if (talking) "TALKING ●" else "TALK (PTT)", color = Color.White, fontWeight = FontWeight.Bold, fontSize = 12.sp)
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
            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            color = Color.White,
            fontWeight = if (active) FontWeight.Bold else FontWeight.Medium,
            fontSize = 11.sp,
        )
    }
}
