package com.lanvid.monitor

import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.TabRowDefaults
import androidx.compose.material3.TabRowDefaults.tabIndicatorOffset
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.lanvid.monitor.ui.ConnectScreen
import com.lanvid.monitor.ui.FxScreen
import com.lanvid.monitor.ui.LanVidMonitorTheme
import com.lanvid.monitor.ui.Lv
import com.lanvid.monitor.ui.MixScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        MonitorEngine.init(this)
        setContent {
            LanVidMonitorTheme {
                val notifPerm = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
                LaunchedEffect(Unit) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        notifPerm.launch(Manifest.permission.POST_NOTIFICATIONS)  // foreground "monitoring" notification
                    }
                }
                var tab by rememberSaveable { mutableIntStateOf(0) }
                val titles = listOf("Підключення", "Channels Mix", "Monitor FX")
                Column(Modifier.fillMaxSize().background(Lv.bg).safeDrawingPadding()) {
                    TabRow(
                        selectedTabIndex = tab, containerColor = Lv.card, contentColor = Lv.text,
                        indicator = { pos -> TabRowDefaults.SecondaryIndicator(Modifier.tabIndicatorOffset(pos[tab]), color = Lv.accent2) },
                    ) {
                        titles.forEachIndexed { i, t ->
                            Tab(selected = tab == i, onClick = { tab = i }, text = { Text(t) })
                        }
                    }
                    when (tab) {
                        0 -> ConnectScreen()
                        1 -> MixScreen()
                        else -> FxScreen()
                    }
                }
            }
        }
    }

    override fun onDestroy() {
        // Keep monitoring when the activity is recreated (rotation); stop only when the user leaves for good.
        if (isFinishing) MonitorEngine.disconnect()
        super.onDestroy()
    }
}
