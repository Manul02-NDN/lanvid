package com.lanvid.monitor

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject

/**
 * App-wide monitor engine (UI thread). All mixing and DSP happen on the PC: every change here is sent
 * immediately as kMonitorControl; the phone only plays the finished stereo bus (native AAudio).
 */
object MonitorEngine {
    private var handle = 0L
    private lateinit var prefs: SharedPreferences
    private lateinit var appContext: Context
    private val main = Handler(Looper.getMainLooper())

    var connected by mutableStateOf<Studio?>(null)
        private set
    val channels = mutableStateListOf<ChannelState>()
    var telemetry by mutableStateOf(Telemetry())
        private set
    var lastError by mutableStateOf<String?>(null)
    var quality by mutableStateOf(StreamQuality.K256)
        private set
    var buffer by mutableStateOf(MonitorBuffer.MS10)
        private set
    var dsp by mutableStateOf(DspSettings())
        private set
    var clientName by mutableStateOf(Build.MODEL ?: "Android")
        private set

    private val names = ByteArray(64 * 16)
    private val peaks = ByteArray(64)
    private val tel = DoubleArray(10)
    private var lastPackets = 0L

    fun init(context: Context) {
        if (handle != 0L) return
        appContext = context.applicationContext
        prefs = appContext.getSharedPreferences("lanvid_monitor", Context.MODE_PRIVATE)
        handle = MonitorNative.nativeCreate()
        quality = StreamQuality.entries.firstOrNull { it.name == prefs.getString("quality", null) } ?: StreamQuality.K256
        buffer = MonitorBuffer.entries.firstOrNull { it.name == prefs.getString("buffer", null) } ?: MonitorBuffer.MS10
        prefs.getString("clientName", null)?.takeIf { it.isNotBlank() }?.let { clientName = it }
        dsp = loadDsp()
    }

    val lastHost: String get() = prefs.getString("lastHost", "") ?: ""
    val lastPort: Int get() = prefs.getInt("lastPort", 5020)

    // ---- Connection ----

    fun connect(studio: Studio) {
        disconnect()
        val rc = MonitorNative.nativeConnect(handle, studio.host, studio.port, clientName.take(15), quality.wireKbps, buffer.ms)
        if (rc != 0 && rc != -2) {
            lastError = "Немає з'єднання з ${studio.host}:${studio.port} (код $rc)"
            return
        }
        lastError = if (rc == -2) "Підключено, але аудіовихід AAudio не відкрився" else null
        connected = studio
        prefs.edit().putString("lastHost", studio.host).putInt("lastPort", studio.port).apply()
        pushDsp()
        channels.forEach { pushMix(it) }
        startService()
        main.removeCallbacks(tick)
        main.post(tick)
    }

    fun disconnect() {
        main.removeCallbacks(tick)
        if (connected != null) MonitorNative.nativeDisconnect(handle)
        connected = null
        telemetry = Telemetry()
        stopService()
    }

    // ---- Stream / mixer / DSP (sent instantly) ----

    fun setQuality(q: StreamQuality) {
        quality = q
        prefs.edit().putString("quality", q.name).apply()
        if (connected != null) MonitorNative.nativeSetStream(handle, q.wireKbps, buffer.ms)
    }

    fun setBuffer(b: MonitorBuffer) {
        buffer = b
        prefs.edit().putString("buffer", b.name).apply()
        if (connected != null) MonitorNative.nativeSetStream(handle, quality.wireKbps, b.ms)
    }

    fun setClientName(n: String) {
        clientName = n.take(15)
        prefs.edit().putString("clientName", clientName).apply()
    }

    fun updateDsp(d: DspSettings) {
        if (d == dsp) return
        dsp = d
        saveDsp(d)
        pushDsp()
    }

    fun setGain(ch: ChannelState, db: Float) { ch.gainDb = db; pushMix(ch); saveMix() }
    fun setPan(ch: ChannelState, pan: Float) { ch.pan = pan; pushMix(ch); saveMix() }
    fun toggleMute(ch: ChannelState) { ch.mute = !ch.mute; pushMix(ch); saveMix() }
    fun toggleSolo(ch: ChannelState) { ch.solo = !ch.solo; pushMix(ch); saveMix() }

    fun resetMix() {
        channels.forEach { it.gainDb = 0f; it.pan = 0f; it.mute = false; it.solo = false; pushMix(it) }
        saveMix()
    }

    private fun pushMix(c: ChannelState) {
        if (connected != null) MonitorNative.nativeSetChannelMix(handle, c.index, c.gainDb, c.pan, c.mute, c.solo)
    }

    private fun pushDsp() {
        if (connected != null) MonitorNative.nativeSetDsp(handle, dsp.params(), dsp.flags())
    }

    // ---- Channel names / VU / telemetry from the PC (15 Hz) ----

    private val tick = object : Runnable {
        override fun run() {
            pullMeta()
            main.postDelayed(this, 66)
        }
    }

    private fun pullMeta() {
        if (connected == null) return
        val n = MonitorNative.nativeGetMeta(handle, names, peaks, tel).coerceIn(0, 64)
        val saved = if (n > channels.size) loadMix() else emptyMap()
        for (i in 0 until n) {
            val raw = names.copyOfRange(i * 16, i * 16 + 16)
            val len = raw.indexOf(0.toByte()).let { if (it < 0) 16 else it }
            val nm = String(raw, 0, len, Charsets.UTF_8).ifBlank { "Ch ${i + 1}" }
            if (i < channels.size) {
                val c = channels[i]
                if (c.name != nm) c.name = nm
                c.peakDbfs = peaks[i].toFloat()
            } else {  // a channel appeared on the PC: restore this phone's last mix for it
                val c = ChannelState(i, nm)
                saved[i]?.let { s -> c.gainDb = s[0]; c.pan = s[1]; c.mute = s[2] != 0f; c.solo = s[3] != 0f }
                c.peakDbfs = peaks[i].toFloat()
                channels.add(c)
                pushMix(c)
            }
        }
        while (channels.size > n) channels.removeAt(channels.size - 1)
        val packets = tel[4].toLong()
        telemetry = Telemetry(
            gainReductionDb = tel[1].toFloat(), mixPeakDbfs = tel[2].toFloat(), bufferMs = tel[3],
            audioPackets = packets, receiving = packets > lastPackets,
            burstFrames = tel[5].toInt(), deviceBufferFrames = tel[6].toInt(), deviceRate = tel[7].toInt(),
            exclusive = tel[8] != 0.0, xruns = tel[9].toInt(),
        )
        lastPackets = packets
    }

    // ---- Foreground service keeps monitoring alive with the screen off ----

    private fun startService() {
        val i = Intent(appContext, MonitorService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) appContext.startForegroundService(i) else appContext.startService(i)
    }

    private fun stopService() {
        if (::appContext.isInitialized) appContext.stopService(Intent(appContext, MonitorService::class.java))
    }

    // ---- Persistence ----

    private fun saveDsp(d: DspSettings) {
        val j = JSONObject()
            .put("hpf", d.hpf.name).put("eqOn", d.eqOn).put("eqLow", d.eqLowDb.toDouble())
            .put("eqMid", d.eqMidDb.toDouble()).put("eqMidHz", d.eqMidHz.toDouble()).put("eqHigh", d.eqHighDb.toDouble())
            .put("compOn", d.compOn).put("thr", d.compThresholdDb.toDouble()).put("ratio", d.compRatio.toDouble())
            .put("makeup", d.compMakeupDb.toDouble()).put("vol", d.masterVolumeDb.toDouble()).put("mute", d.masterMute)
        prefs.edit().putString("dsp", j.toString()).apply()
    }

    private fun loadDsp(): DspSettings {
        val s = prefs.getString("dsp", null) ?: return DspSettings()
        return runCatching {
            val j = JSONObject(s)
            DspSettings(
                hpf = HpfMode.entries.firstOrNull { it.name == j.optString("hpf") } ?: HpfMode.OFF,
                eqOn = j.optBoolean("eqOn", true), eqLowDb = j.optDouble("eqLow", 0.0).toFloat(),
                eqMidDb = j.optDouble("eqMid", 0.0).toFloat(), eqMidHz = j.optDouble("eqMidHz", 1200.0).toFloat(),
                eqHighDb = j.optDouble("eqHigh", 0.0).toFloat(), compOn = j.optBoolean("compOn", false),
                compThresholdDb = j.optDouble("thr", -18.0).toFloat(), compRatio = j.optDouble("ratio", 3.0).toFloat(),
                compMakeupDb = j.optDouble("makeup", 0.0).toFloat(), masterVolumeDb = j.optDouble("vol", 0.0).toFloat(),
                masterMute = j.optBoolean("mute", false),
            )
        }.getOrDefault(DspSettings())
    }

    private fun saveMix() {
        val j = JSONObject()
        channels.forEach { c ->
            j.put(c.index.toString(), "${c.gainDb},${c.pan},${if (c.mute) 1 else 0},${if (c.solo) 1 else 0}")
        }
        prefs.edit().putString("mix", j.toString()).apply()
    }

    private fun loadMix(): Map<Int, List<Float>> {
        val s = prefs.getString("mix", null) ?: return emptyMap()
        return runCatching {
            val j = JSONObject(s)
            j.keys().asSequence().associate { k ->
                k.toInt() to j.getString(k).split(",").map { it.toFloat() }
            }.filterValues { it.size == 4 }
        }.getOrDefault(emptyMap())
    }
}
