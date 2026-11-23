package com.lanvid.monitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.monitor.MonitorEngine
import com.lanvid.monitor.Studio
import com.lanvid.monitor.StudioDiscovery

@Composable
fun ConnectScreen() {
    val context = LocalContext.current
    val discovery = remember { StudioDiscovery(context) }
    DisposableEffect(Unit) {
        discovery.start()
        onDispose { discovery.stop() }
    }
    var host by remember { mutableStateOf(MonitorEngine.lastHost) }
    var port by remember { mutableStateOf(MonitorEngine.lastPort.toString()) }
    val connected = MonitorEngine.connected
    val tel = MonitorEngine.telemetry

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        if (connected != null) {
            SectionCard("Підключено") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(10.dp).background(if (tel.receiving) Lv.green else Lv.amber, CircleShape))
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text(connected.name, color = Lv.text, fontWeight = FontWeight.SemiBold)
                        Text("${connected.host}:${connected.port}", color = Lv.muted, fontSize = 12.sp)
                    }
                    Button(onClick = { MonitorEngine.disconnect() }, colors = ButtonDefaults.buttonColors(containerColor = Lv.red)) {
                        Text("Відключити")
                    }
                }
                Text(
                    if (tel.receiving) "Звук надходить · буфер %.1f мс".format(tel.bufferMs) else "Очікування звуку від LanVid Studio…",
                    color = Lv.muted, fontSize = 13.sp,
                )
                if (tel.burstFrames > 0) {
                    Text(
                        "AAudio ${if (tel.exclusive) "EXCLUSIVE" else "SHARED"} · burst ${tel.burstFrames} · буфер " +
                            "${tel.deviceBufferFrames} кадрів @ ${tel.deviceRate} Гц · xrun ${tel.xruns}",
                        color = Lv.muted, fontSize = 11.sp,
                    )
                }
            }
        }

        SectionCard("LanVid Studio у мережі", trailing = {
            TextButton(onClick = { discovery.start() }) { Text("Оновити", color = Lv.accent2) }
        }) {
            if (discovery.studios.isEmpty()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Lv.accent2, strokeWidth = 2.dp)
                    Spacer(Modifier.width(10.dp))
                    Text(discovery.status.ifEmpty { "Пошук…" }, color = Lv.muted, fontSize = 13.sp)
                }
                Text(
                    "У LanVid Studio на ПК увімкніть «Мобільний моніторинг» — студія з'явиться тут автоматично.",
                    color = Lv.muted, fontSize = 12.sp,
                )
            }
            discovery.studios.forEach { s ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Lv.cardHi, RoundedCornerShape(12.dp))
                        .clickable { MonitorEngine.connect(s) }
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(s.name, color = Lv.text)
                        Text("${s.host}:${s.port}", color = Lv.muted, fontSize = 12.sp)
                    }
                    Text(if (connected?.id == s.id) "Підключено" else "Підключити", color = Lv.accent2, fontWeight = FontWeight.SemiBold)
                }
            }
        }

        SectionCard("Підключення за IP") {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = host, onValueChange = { host = it.trim() }, label = { Text("IP ПК") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri), modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = port, onValueChange = { port = it.filter(Char::isDigit).take(5) }, label = { Text("Порт") },
                    singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.width(96.dp),
                )
            }
            Button(
                onClick = { MonitorEngine.connect(Studio("LanVid Studio ($host)", host, port.toIntOrNull() ?: 5020)) },
                enabled = host.isNotEmpty(), modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.buttonColors(containerColor = Lv.accent),
            ) { Text("Підключити") }
        }

        SectionCard("Цей телефон") {
            OutlinedTextField(
                value = MonitorEngine.clientName, onValueChange = { MonitorEngine.changeClientName(it) },
                label = { Text("Назва у списку LanVid Studio") }, singleLine = true, modifier = Modifier.fillMaxWidth(),
            )
            Text("До 15 символів — так телефон підписаний у списку смартфонів на ПК.", color = Lv.muted, fontSize = 12.sp)
        }

        MonitorEngine.lastError?.let { Text(it, color = Lv.red, fontSize = 13.sp) }
    }
}
