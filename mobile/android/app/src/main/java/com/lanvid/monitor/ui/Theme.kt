package com.lanvid.monitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Soft lavender palette matching LanVid Studio. */
object Lv {
    val bg = Color(0xFF12101F)
    val card = Color(0xFF1F1C36)
    val cardHi = Color(0xFF2B264A)
    val accent = Color(0xFF8B7CF6)
    val accent2 = Color(0xFFA78BFA)
    val text = Color(0xFFF3F2F8)
    val muted = Color(0xFF9A94B3)
    val green = Color(0xFF10B981)
    val amber = Color(0xFFF59E0B)
    val red = Color(0xFFEF4444)

    fun meter(dbfs: Float) = if (dbfs > -3f) red else if (dbfs > -12f) amber else green
}

@Composable
fun LanVidMonitorTheme(content: @Composable () -> Unit) = MaterialTheme(
    colorScheme = darkColorScheme(
        primary = Lv.accent, secondary = Lv.accent2, background = Lv.bg, surface = Lv.card,
        onPrimary = Color.White, onBackground = Lv.text, onSurface = Lv.text, error = Lv.red,
    ),
    content = content,
)

@Composable
fun SectionCard(
    title: String,
    modifier: Modifier = Modifier,
    trailing: (@Composable () -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier
            .fillMaxWidth()
            .background(Lv.card, RoundedCornerShape(16.dp))
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(title.uppercase(), color = Lv.accent2, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f))
            trailing?.invoke()
        }
        content()
    }
}
