package com.lanvid.monitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.monitor.DspSettings
import com.lanvid.monitor.HpfMode
import com.lanvid.monitor.MonitorEngine

/** Monitor FX — Server DSP: runs on the PC, in real time, only on this phone's personal bus. */
@Composable
fun FxScreen() {
    val d = MonitorEngine.dsp
    val tel = MonitorEngine.telemetry
    val enabled = MonitorEngine.connected != null
    fun set(n: DspSettings) = MonitorEngine.updateDsp(n)

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp).alpha(if (enabled) 1f else 0.5f),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Обробка виконується на ПК у реальному часі лише для вашої моніторної шини.", color = Lv.muted, fontSize = 12.sp)

        SectionCard("High-Pass Filter") {
            SegmentRow(HpfMode.entries, d.hpf, { it.label }) { set(d.copy(hpf = it)) }
        }

        SectionCard("3-смуговий еквалайзер", Modifier.alpha(if (d.eqOn) 1f else 0.6f), trailing = {
            Switch(d.eqOn, { set(d.copy(eqOn = it)) }, colors = SwitchDefaults.colors(checkedTrackColor = Lv.accent))
        }) {
            ParamSlider("Low (100 Гц)", d.eqLowDb, -12f..12f, { set(d.copy(eqLowDb = it)) })
            ParamSlider("Mid", d.eqMidDb, -12f..12f, { set(d.copy(eqMidDb = it)) })
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                DspSettings.midFrequencies.forEach { f ->
                    val sel = d.eqMidHz == f
                    Box(
                        Modifier.weight(1f).heightIn(min = 30.dp)
                            .background(if (sel) Lv.accent else Lv.cardHi, RoundedCornerShape(8.dp))
                            .clickable { set(d.copy(eqMidHz = f)) }.padding(vertical = 6.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(if (f >= 1000f) "%.1fk".format(f / 1000f) else f.toInt().toString(),
                            color = if (sel) Color.White else Lv.text, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                    }
                }
            }
            ParamSlider("High (8 кГц)", d.eqHighDb, -12f..12f, { set(d.copy(eqHighDb = it)) })
        }

        SectionCard("Компресор", Modifier.alpha(if (d.compOn) 1f else 0.6f), trailing = {
            Switch(d.compOn, { set(d.copy(compOn = it)) }, colors = SwitchDefaults.colors(checkedTrackColor = Lv.accent))
        }) {
            ParamSlider("Threshold", d.compThresholdDb, -40f..0f, { set(d.copy(compThresholdDb = it)) })
            ParamSlider("Ratio", d.compRatio, 1f..10f, { set(d.copy(compRatio = it)) }, step = 0.1f, format = "%.1f : 1")
            ParamSlider("Makeup Gain", d.compMakeupDb, 0f..18f, { set(d.copy(compMakeupDb = it)) })
            GainReductionMeter(if (d.compOn) tel.gainReductionDb else 0f)
        }

        SectionCard("Master Limiter & Volume") {
            Text("Лімітер −0.5 dBFS завжди увімкнений — захищає слух від різких піків.", color = Lv.muted, fontSize = 12.sp)
            ParamSlider("Гучність у навушниках", d.masterVolumeDb, -60f..12f, { set(d.copy(masterVolumeDb = it)) })
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Вихід", color = Lv.muted, fontSize = 12.sp)
                VuMeter(tel.mixPeakDbfs, Modifier.weight(1f).height(8.dp), vertical = false)
                Text("%.0f dBFS".format(tel.mixPeakDbfs), color = Lv.muted, fontSize = 11.sp, modifier = Modifier.width(64.dp))
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Mute (вимкнути звук)", color = Lv.text, modifier = Modifier.weight(1f))
                Switch(d.masterMute, { set(d.copy(masterMute = it)) }, colors = SwitchDefaults.colors(checkedTrackColor = Lv.red))
            }
        }

        TextButton(onClick = { set(DspSettings()) }) { Text("Скинути обробку", color = Lv.accent2) }
    }
}

@Composable
private fun GainReductionMeter(grDb: Float) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Gain Reduction", color = Lv.muted, fontSize = 12.sp)
        Box(Modifier.weight(1f).height(8.dp).background(Lv.cardHi, RoundedCornerShape(3.dp)), contentAlignment = Alignment.CenterEnd) {
            val t = (-grDb / 24f).coerceIn(0f, 1f)
            Box(Modifier.fillMaxWidth(t).height(8.dp).background(Lv.amber, RoundedCornerShape(3.dp)))
        }
        Text("%.1f дБ".format(grDb), color = Lv.amber, fontSize = 11.sp, modifier = Modifier.width(56.dp))
    }
}
