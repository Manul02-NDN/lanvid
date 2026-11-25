package com.lanvid.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.*
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.lanvid.app.ui.BroadcastScreen
import com.lanvid.app.ui.LanVidBroadcastTheme

class MainActivity : ComponentActivity() {
    private lateinit var cameraStreamer: Camera2Streamer
    private val micStreamer = AudioInputStreamer()

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        LanVidNative.nativeInit()
        cameraStreamer = Camera2Streamer(this)

        setContent {
            LanVidBroadcastTheme {
                var hasCamPerm by remember {
                    mutableStateOf(
                        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                    )
                }
                val permLauncher = rememberLauncherForActivityResult(
                    ActivityResultContracts.RequestMultiplePermissions()
                ) { grants ->
                    hasCamPerm = grants[Manifest.permission.CAMERA] == true ||
                        checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                }
                LaunchedEffect(Unit) {
                    val perms = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        perms += Manifest.permission.POST_NOTIFICATIONS
                    }
                    permLauncher.launch(perms.toTypedArray())
                }
                BroadcastScreen(
                    cameraStreamer = cameraStreamer,
                    micStreamer = micStreamer,
                    hasCameraPermission = hasCamPerm
                )
            }
        }
    }

    override fun onDestroy() {
        cameraStreamer.release()
        micStreamer.stop()
        LanVidNative.nativeStopSender()
        LanVidNative.nativeStopReceiver()
        LanVidNative.nativeSetReceiverSurface(null)
        super.onDestroy()
    }
}
