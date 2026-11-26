package com.lanvid.intercom

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

enum class BeltpackChannel(val label: String) {
    DIRECTOR("Director"),
    GROUP_A("Group A"),
    GROUP_B("Group B"),
    ALL_CALL("All Call")
}

/**
 * Beltpack mobile UI backed by `LanVidCore` C ABI (`lv_tally_intercom_*`, UDP :5024).
 */
@Composable
fun BeltpackIntercomScreen(
    deskHost: String = "192.168.1.10",
    deskPort: Int = 5024,
    tallyState: Int = 0, // 0=STBY, 1=PVW, 2=PGM ON AIR
    onTalkChanged: (Boolean) -> Unit = {},
    onChannelSelected: (BeltpackChannel) -> Unit = {},
    onVolumeChanged: (Float) -> Unit = {}
) {
    var selectedChannel by remember { mutableStateOf(BeltpackChannel.DIRECTOR) }
    var pttHeld by remember { mutableStateOf(false) }
    var latched by remember { mutableStateOf(false) }
    var volume by remember { mutableFloatStateOf(1.0f) }

    val tallyColor = when (tallyState) {
        2 -> Color(0xFFEF4444)
        1 -> Color(0xFF10B981)
        else -> Color(0xFF8B5CF6)
    }
    val tallyTitle = when (tallyState) {
        2 -> "ON AIR / PGM"
        1 -> "PREVIEW"
        else -> "STANDBY"
    }
    val micOpen = pttHeld || latched

    LaunchedEffect(micOpen) { onTalkChanged(micOpen) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF121020))
            .padding(20.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.SpaceBetween
    ) {
        Surface(
            modifier = Modifier.fillMaxWidth().height(84.dp),
            shape = RoundedCornerShape(18.dp),
            color = tallyColor.copy(alpha = 0.18f),
            border = BorderStroke(3.dp, tallyColor)
        ) {
            Column(
                modifier = Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text(tallyTitle, color = tallyColor, fontSize = 22.sp, fontWeight = FontWeight.Black)
                Text("$deskHost:$deskPort", color = Color(0xFFA5A1C4), fontSize = 12.sp)
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BeltpackChannel.entries.forEach { ch ->
                FilterChip(
                    selected = selectedChannel == ch,
                    onClick = {
                        selectedChannel = ch
                        onChannelSelected(ch)
                    },
                    label = { Text(ch.label) }
                )
            }
        }

        Box(
            modifier = Modifier
                .size(200.dp)
                .clip(CircleShape)
                .background(if (micOpen) Color(0xFFEF4444) else Color(0xFF8B5CF6))
                .pointerInput(Unit) {
                    detectTapGestures(
                        onPress = {
                            pttHeld = true
                            tryAwaitRelease()
                            pttHeld = false
                        }
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = if (micOpen) "TALKING" else "HOLD PTT",
                color = Color.White,
                fontSize = 20.sp,
                fontWeight = FontWeight.Bold
            )
        }

        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("LATCH Mic", color = Color.White)
                Switch(checked = latched, onCheckedChange = { latched = it })
            }
            Text("Headphones: ${(volume * 100).toInt()}%", color = Color(0xFFA5A1C4), fontSize = 12.sp)
            Slider(
                value = volume,
                onValueChange = {
                    volume = it
                    onVolumeChanged(it)
                },
                valueRange = 0f..2.0f
            )
        }
    }
}
