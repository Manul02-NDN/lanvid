package com.lanvid.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * LanVid Studio neutral dark design system (1:1 match with LanVid Studio desktop/web reference):
 * neutral charcoal surfaces (#1F1F1F / #262626 / #2E2E2E), hairline borders, single blue accent (#0A84FF),
 * quiet sans-serif typography + tabular monospace numerals.
 */
object Studio {
    val bg = Color(0xFF1F1F1F)
    val bgSubtle = Color(0xFF181818)
    val surface = Color(0xFF262626)
    val surface2 = Color(0xFF2E2E2E)
    val surface3 = Color(0xFF3A3A3A)
    val elevated = Color(0xFF2C2C2C)
    val canvas = Color(0xFF1A1A1A)
    val segActive = Color(0xFF444446)

    val hudBar = Color(0xE01F1F1F)
    val hudSurface = Color(0xEB262626)

    val border = Color(0x14FFFFFF)
    val borderStrong = Color(0x24FFFFFF)

    val text = Color(0xFFECECEC)
    val text2 = Color(0xFFA8A8AD)
    val text3 = Color(0xFF76767B)

    val primaryBg = Color(0xFFECECEC)
    val primaryFg = Color(0xFF121212)

    val accent = Color(0xFF0A84FF)
    val accentSoft = Color(0x290A84FF)
    val green = Color(0xFF30D158)
    val greenSoft = Color(0x2630D158)
    val amber = Color(0xFFFF9F0A)
    val amberSoft = Color(0x26FF9F0A)
    val violet = Color(0xFFBF5AF2)
    val violetSoft = Color(0x26BF5AF2)
    val red = Color(0xFFFF453A)
    val redSoft = Color(0x29FF453A)
    val yellow = Color(0xFFFFD60A)
}

/** Compatibility palette mapped cleanly to LanVid Studio neutral tokens. */
object Bm {
    val bg = Studio.canvas
    val panel = Studio.surface
    val panelHi = Studio.surface2
    val text = Studio.text
    val dim = Studio.text2
    val accent = Studio.accent
    val live = Studio.red
    val good = Studio.green
    val warn = Studio.amber

    fun sans(size: TextUnit, weight: FontWeight = FontWeight.Medium) =
        TextStyle(fontFamily = FontFamily.SansSerif, fontSize = size, fontWeight = weight)

    fun mono(size: TextUnit, weight: FontWeight = FontWeight.SemiBold) =
        TextStyle(fontFamily = FontFamily.Monospace, fontSize = size, fontWeight = weight)
}

@Composable
fun LanVidBroadcastTheme(content: @Composable () -> Unit) =
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Studio.accent,
            onPrimary = Color.White,
            background = Studio.bg,
            surface = Studio.surface,
            surfaceVariant = Studio.surface2,
            onBackground = Studio.text,
            onSurface = Studio.text,
            outline = Studio.borderStrong,
            error = Studio.red
        ),
        content = content
    )

@Composable
fun Chip(text: String, color: Color = Studio.text, modifier: Modifier = Modifier) =
    Text(
        text = text,
        style = Bm.mono(10.5.sp, FontWeight.SemiBold),
        color = color,
        maxLines = 1,
        softWrap = false,
        modifier = modifier
            .background(Studio.surface2, RoundedCornerShape(7.dp))
            .border(0.75.dp, Studio.border, RoundedCornerShape(7.dp))
            .padding(horizontal = 7.dp, vertical = 3.dp)
    )

@Composable
fun StudioStatusPill(
    dotColor: Color,
    title: String,
    detail: String? = null,
    modifier: Modifier = Modifier
) {
    Surface(
        color = Studio.surface2,
        shape = RoundedCornerShape(999.dp),
        border = BorderStroke(0.75.dp, Studio.border),
        modifier = modifier
    ) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Box(
                Modifier
                    .size(12.dp)
                    .clip(CircleShape)
                    .background(dotColor.copy(alpha = 0.24f)),
                contentAlignment = Alignment.Center
            ) {
                Box(
                    Modifier
                        .size(7.dp)
                        .clip(CircleShape)
                        .background(dotColor)
                )
            }
            Text(
                text = title,
                style = Bm.sans(11.5.sp, FontWeight.SemiBold),
                color = Studio.text,
                maxLines = 1,
                softWrap = false
            )
            if (!detail.isNullOrBlank()) {
                Text(
                    text = detail,
                    style = Bm.mono(10.5.sp, FontWeight.Medium),
                    color = Studio.text3,
                    maxLines = 1,
                    softWrap = false
                )
            }
        }
    }
}

fun Modifier.clickableNoRipple(onClick: () -> Unit): Modifier =
    this.then(Modifier.clickable(interactionSource = null, indication = null, onClick = onClick))
