package com.lanvid.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Blackmagic-style broadcast HUD palette (matching NDICam). */
object Bm {
    val bg = Color.Black
    val panel = Color(0xFF171717)
    val panelHi = Color(0xFF292929)
    val text = Color.White
    val dim = Color(0xFF8C8C8C)
    val accent = Color(0xFFFFC700)
    val live = Color(0xFFF22626)
    val good = Color(0xFF33D966)
    val warn = Color(0xFFFF9900)

    fun mono(size: TextUnit, weight: FontWeight = FontWeight.SemiBold) =
        TextStyle(fontFamily = FontFamily.Monospace, fontSize = size, fontWeight = weight)
}

@Composable
fun LanVidBroadcastTheme(content: @Composable () -> Unit) =
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Bm.accent,
            onPrimary = Color.Black,
            background = Bm.bg,
            surface = Bm.panel,
            onBackground = Bm.text,
            onSurface = Bm.text
        ),
        content = content
    )

@Composable
fun Chip(text: String, color: Color = Bm.text, modifier: Modifier = Modifier) =
    Text(
        text = text,
        style = Bm.mono(11.sp),
        color = color,
        maxLines = 1,
        softWrap = false,
        modifier = modifier
            .background(Bm.panelHi, RoundedCornerShape(4.dp))
            .padding(horizontal = 6.dp, vertical = 3.dp)
    )

fun Modifier.clickableNoRipple(onClick: () -> Unit): Modifier =
    this.then(Modifier.clickable(interactionSource = null, indication = null, onClick = onClick))
