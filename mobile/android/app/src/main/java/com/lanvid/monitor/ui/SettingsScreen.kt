package com.lanvid.monitor.ui

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.lanvid.monitor.BuildConfig
import com.lanvid.monitor.MonitorEngine
import com.lanvid.monitor.R

@Composable
fun SettingsScreen() {
    val context = LocalContext.current
    fun openUrl(url: String) {
        runCatching {
            context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        SectionCard(stringResource(R.string.demo_title)) {
            Text(stringResource(R.string.demo_desc), color = Lv.muted, fontSize = 13.sp)
            if (MonitorEngine.isDemo) {
                Button(
                    onClick = { MonitorEngine.disconnect() },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = Lv.red),
                ) {
                    Text(stringResource(R.string.demo_exit))
                }
            } else {
                Button(
                    onClick = { MonitorEngine.connectDemo() },
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = Lv.accent),
                ) {
                    Text(stringResource(R.string.demo_open))
                }
            }
        }

        SectionCard(stringResource(R.string.settings_about)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("LanVid Monitor", color = Lv.text, fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                Text("v${BuildConfig.VERSION_NAME}", color = Lv.accent2, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
            }
            Text(
                "Opus CELT 48 kHz · Native AAudio · 64ch->Stereo Server DSP",
                color = Lv.muted,
                fontSize = 12.sp,
            )
            Text(
                stringResource(R.string.settings_lan_only),
                color = Lv.muted,
                fontSize = 12.sp,
            )
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedButton(
                    onClick = { openUrl("https://manul02-ndn.github.io/lanvid/privacy-policy.html") },
                    modifier = Modifier.weight(1f),
                ) {
                    Text(stringResource(R.string.settings_privacy), color = Lv.accent2, fontSize = 12.sp)
                }
                OutlinedButton(
                    onClick = { openUrl("https://manul02-ndn.github.io/lanvid/support.html") },
                    modifier = Modifier.weight(1f),
                ) {
                    Text(stringResource(R.string.settings_support), color = Lv.accent2, fontSize = 12.sp)
                }
            }
        }
    }
}
