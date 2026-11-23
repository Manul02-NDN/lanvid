package com.lanvid.monitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.monitor.ChannelState
import com.lanvid.monitor.MonitorBuffer
import com.lanvid.monitor.MonitorEngine
import com.lanvid.monitor.StreamQuality

@Composable
fun MixScreen() {
    Column(Modifier.fillMaxSize().padding(vertical = 16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SectionCard("Моніторинговий стереопотік", Modifier.padding(horizontal = 16.dp)) {
            Text("Якість", color = Lv.muted, fontSize = 12.sp)
            SegmentRow(StreamQuality.entries, MonitorEngine.quality, { it.label }) { MonitorEngine.setQuality(it) }
            Text(MonitorEngine.quality.detail, color = Lv.muted, fontSize = 11.sp)
            Text("Буфер", color = Lv.muted, fontSize = 12.sp)
            SegmentRow(MonitorBuffer.entries, MonitorEngine.buffer, { "${it.ms} мс" }) { MonitorEngine.setBuffer(it) }
        }
        when {
            MonitorEngine.connected == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text("Підключіться до LanVid Studio на вкладці «Підключення».", color = Lv.muted)
            }
            MonitorEngine.channels.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = Lv.accent2)
            }
            else -> {
                val anySolo = MonitorEngine.channels.any { it.solo }
                Row(
                    Modifier.horizontalScroll(rememberScrollState()).padding(horizontal = 16.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    MonitorEngine.channels.forEach { ChannelStrip(it, anySolo) }
                }
                Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("Мікс виконується на ПК — у телефон надходить готове стерео.", color = Lv.muted, fontSize = 11.sp,
                        modifier = Modifier.weight(1f))
                    TextButton(onClick = { MonitorEngine.resetMix() }) { Text("Скинути", color = Lv.accent2) }
                }
            }
        }
    }
}

@Composable
private fun ChannelStrip(ch: ChannelState, anySolo: Boolean) {
    val audible = !ch.mute && (!anySolo || ch.solo)
    Column(
        Modifier
            .width(92.dp)
            .background(Lv.card, RoundedCornerShape(14.dp))
            .padding(8.dp)
            .alpha(if (audible) 1f else 0.6f),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(ch.name, color = Lv.text, fontWeight = FontWeight.SemiBold, fontSize = 12.sp, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
        Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            VuMeter(ch.peakDbfs, Modifier.width(6.dp).height(190.dp))
            VerticalFader(ch.gainDb, { MonitorEngine.setGain(ch, it) }, if (audible) Lv.accent else Lv.muted)
        }
        Text("%+.1f".format(ch.gainDb), color = Lv.muted, fontSize = 11.sp)
        PanControl(ch.pan, { MonitorEngine.setPan(ch, it) })
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            PillToggle("M", ch.mute, Lv.red) { MonitorEngine.toggleMute(ch) }
            PillToggle("S", ch.solo, Lv.amber) { MonitorEngine.toggleSolo(ch) }
        }
    }
}
